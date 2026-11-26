#define _POSIX_C_SOURCE 200809L

#include "sensor_loop.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "msg.h"
#include "sock.h"

#define NS_PER_SEC 1000000000LL

static int now_ns(clockid_t id, int64_t *out)
{
    struct timespec ts;
    if (clock_gettime(id, &ts) < 0)
        return -errno;
    *out = (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
    return 0;
}

/* Sleep until CLOCK_MONOTONIC reaches deadline. 0, -EINTR or -errno. */
static int sleep_until(int64_t deadline)
{
    struct timespec ts = {
        .tv_sec  = (time_t)(deadline / NS_PER_SEC),
        .tv_nsec = (long)(deadline % NS_PER_SEC),
    };
    return -clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

/*
 * Index of the next tick after tick to run: tick + 1, or the first tick
 * still in the future if now is already past that. Adds skipped ticks
 * to *skipped.
 */
static uint64_t next_tick(int64_t t0, int64_t interval, uint64_t tick, int64_t now,
                          uint64_t *skipped)
{
    uint64_t next = tick + 1;
    if (now >= t0 + (int64_t)next * interval) {
        uint64_t future = (uint64_t)((now - t0) / interval) + 1;
        *skipped += future - next;
        next = future;
    }
    return next;
}

static int send_heartbeat(const struct sensor_loop_cfg *cfg, int fd, pid_t pid,
                          struct sensor_loop_stats *st)
{
    struct msg m;
    memset(&m, 0, sizeof m);
    m.type = MSG_HEARTBEAT;
    m.u.heartbeat.pid = (int32_t)pid;
    m.u.heartbeat.reading_count = st->readings;
    m.u.heartbeat.sensor_id = (uint32_t)cfg->kind;
    int rc = sock_send(fd, &m);
    if (rc == 0)
        st->heartbeats++;
    return rc;
}

static int send_reading(const struct sensor_loop_cfg *cfg, struct backend *b, int fd,
                        struct sensor_loop_stats *st)
{
    struct backend_reading r = backend_read(b);

    struct msg m;
    memset(&m, 0, sizeof m);
    m.type = MSG_READING;
    m.u.reading.sensor_id = (uint32_t)cfg->kind;
    m.u.reading.seq = st->readings;
    int rc = now_ns(CLOCK_REALTIME, &m.u.reading.timestamp_ns);
    if (rc == 0)
        rc = now_ns(CLOCK_MONOTONIC, &m.u.reading.mono_ns);
    if (rc < 0) {
        log_error(EV_SYSCALL_FAIL, "op=clock_gettime err=%s", strerror(-rc));
        return rc;
    }
    if (b->ops != NULL && b->ops->simulated)
        m.u.reading.flags |= MSG_FLAG_SIMULATED;
    if (r.status == BACKEND_OK) {
        m.u.reading.value = r.value;
    } else {
        m.u.reading.flags |= MSG_FLAG_SENSOR_ERROR;
        log_warn(EV_SENSOR_ERROR, "op=read seq=%u err=%s", st->readings, strerror(r.err));
    }

    rc = sock_send(fd, &m);
    if (rc == 0)
        st->readings++;
    return rc;
}

int sensor_loop(const struct sensor_loop_cfg *cfg, struct backend *b, int fd,
                struct sensor_loop_stats *st)
{
    memset(st, 0, sizeof *st);
    if (cfg->period_ns <= 0 || cfg->heartbeat_ns <= 0 || sensor_kind_name(cfg->kind) == NULL)
        return -EINVAL;

    const pid_t pid = getpid();
    int64_t t0;
    int rc = now_ns(CLOCK_MONOTONIC, &t0);
    if (rc < 0) {
        log_error(EV_SYSCALL_FAIL, "op=clock_gettime err=%s", strerror(-rc));
        return rc;
    }

    /* First heartbeat right away, so liveness never waits on a sample. */
    rc = send_heartbeat(cfg, fd, pid, st);
    if (rc != 0)
        return rc;
    uint64_t k = 0; /* next sample tick */
    uint64_t j = 1; /* next heartbeat tick */

    for (;;) {
        if (cfg->stop != NULL && *cfg->stop)
            return 0;

        int64_t sample_at = t0 + (int64_t)k * cfg->period_ns;
        int64_t beat_at = t0 + (int64_t)j * cfg->heartbeat_ns;
        rc = sleep_until(sample_at < beat_at ? sample_at : beat_at);
        if (rc == -EINTR)
            continue; /* re-check *stop; deadlines are unchanged */
        if (rc < 0) {
            log_error(EV_SYSCALL_FAIL, "op=clock_nanosleep err=%s", strerror(-rc));
            return rc;
        }

        int64_t now;
        if ((rc = now_ns(CLOCK_MONOTONIC, &now)) < 0) {
            log_error(EV_SYSCALL_FAIL, "op=clock_gettime err=%s", strerror(-rc));
            return rc;
        }

        if (now >= sample_at) {
            rc = send_reading(cfg, b, fd, st);
            if (rc != 0)
                return rc;
            if (cfg->count != 0 && st->readings >= cfg->count)
                return 0;

            int64_t after;
            if ((rc = now_ns(CLOCK_MONOTONIC, &after)) < 0) {
                log_error(EV_SYSCALL_FAIL, "op=clock_gettime err=%s", strerror(-rc));
                return rc;
            }
            uint64_t skipped = 0;
            uint64_t next = next_tick(t0, cfg->period_ns, k, after, &skipped);
            if (skipped != 0) {
                int64_t late = after - (t0 + (int64_t)(k + 1) * cfg->period_ns);
                log_warn(EV_SAMPLE_OVERRUN, "missed=%llu late_ms=%lld seq=%u",
                         (unsigned long long)skipped, (long long)(late / 1000000),
                         st->readings - 1);
                st->missed_ticks += skipped;
            }
            k = next;
        }

        if (now >= beat_at) {
            rc = send_heartbeat(cfg, fd, pid, st);
            if (rc != 0)
                return rc;
            int64_t after;
            if ((rc = now_ns(CLOCK_MONOTONIC, &after)) < 0) {
                log_error(EV_SYSCALL_FAIL, "op=clock_gettime err=%s", strerror(-rc));
                return rc;
            }
            uint64_t ignored = 0;
            j = next_tick(t0, cfg->heartbeat_ns, j, after, &ignored);
        }
    }
}
