#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"
#include "msg.h"
#include "sensor_loop.h"
#include "sock.h"

/*
 * sensor_loop() runs in a forked child, sending over a SEQPACKET socketpair.
 * The parent reads each message as it arrives and stamps its own
 * CLOCK_MONOTONIC arrival time, as the hub will. Tolerances are generous
 * (the design's 50 ms jitter budget) so the tests also pass under valgrind.
 */

#define MS        1000000LL
#define TOLERANCE (50 * MS)
#define MAX_MSGS  256

/* Child exit codes. */
#define CHILD_OK           0
#define CHILD_DISCONNECTED 10
#define CHILD_ERROR        20

struct got {
    struct msg m;
    int64_t arrival_ns;
};

struct run {
    struct got msgs[MAX_MSGS];
    int n;
    struct sensor_loop_stats stats;
    int exit_code;
    pid_t pid;
    int64_t elapsed_ns; /* fork to reap */
};

static int64_t mono_now(void)
{
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_ns(int64_t ns)
{
    struct timespec ts = { .tv_sec = (time_t)(ns / 1000000000LL),
                           .tv_nsec = (long)(ns % 1000000000LL) };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

/* ---- test-only backends ------------------------------------------------ */

static int64_t slow_read_ns;  /* every read takes this long */
static int     stall_on_read; /* this read (0-based) also stalls ... */
static int64_t stall_ns;      /* ... for this long */
static int     reads_done;

static int test_init(void **state, enum sensor_kind kind)
{
    (void)kind;
    *state = NULL;
    reads_done = 0;
    return 0;
}

static struct backend_reading test_read(void *state)
{
    (void)state;
    if (slow_read_ns > 0)
        sleep_ns(slow_read_ns);
    if (reads_done == stall_on_read && stall_ns > 0)
        sleep_ns(stall_ns);
    reads_done++;
    return (struct backend_reading){ .status = BACKEND_OK, .value = (double)reads_done };
}

static int test_close(void *state)
{
    (void)state;
    return 0;
}

static const struct backend_ops test_ops = {
    .name = "test", .simulated = 1,
    .init = test_init, .read = test_read, .close = test_close,
};

/* ---- harness ----------------------------------------------------------- */

static volatile sig_atomic_t child_stop;

static void on_alarm(int sig)
{
    (void)sig;
    child_stop = 1;
}

/*
 * Run sensor_loop in a child. ops == NULL uses the fake backend.
 * stop_after_ns > 0 sets *stop from SIGALRM after that long.
 * close_early closes the parent's end before reading anything.
 */
static void run_loop(struct run *out, const struct sensor_loop_cfg *cfg_in,
                     const struct backend_ops *ops, int64_t stop_after_ns,
                     int close_early)
{
    memset(out, 0, sizeof *out);
    int sv[2], stats_pipe[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    assert(pipe(stats_pipe) == 0);

    int64_t start = mono_now();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        close(stats_pipe[0]);
        struct backend b;
        if (ops == NULL) {
            if (backend_init(&b, "fake", cfg_in->kind) < 0)
                _exit(CHILD_ERROR);
        } else {
            void *state = NULL;
            if (ops->init(&state, cfg_in->kind) < 0)
                _exit(CHILD_ERROR);
            b.ops = ops;
            b.state = state;
        }

        struct sensor_loop_cfg cfg = *cfg_in;
        cfg.stop = &child_stop;
        if (stop_after_ns > 0) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = on_alarm; /* no SA_RESTART */
            sigemptyset(&sa.sa_mask);
            if (sigaction(SIGALRM, &sa, NULL) < 0)
                _exit(CHILD_ERROR);
            struct itimerval it = { .it_value = {
                .tv_sec = (time_t)(stop_after_ns / 1000000000LL),
                .tv_usec = (suseconds_t)(stop_after_ns % 1000000000LL / 1000) } };
            if (setitimer(ITIMER_REAL, &it, NULL) < 0)
                _exit(CHILD_ERROR);
        }

        struct sensor_loop_stats st;
        int rc = sensor_loop(&cfg, &b, sv[1], &st);
        int code = rc == 0 ? CHILD_OK : rc == SOCK_DISCONNECTED ? CHILD_DISCONNECTED : CHILD_ERROR;
        if (write(stats_pipe[1], &st, sizeof st) != (ssize_t)sizeof st)
            code = CHILD_ERROR;
        /* Leave no fds open: valgrind --track-fds counts them as errors. */
        if (backend_close(&b) < 0 || close(sv[1]) < 0 || close(stats_pipe[1]) < 0)
            code = CHILD_ERROR;
        _exit(code);
    }
    out->pid = pid;
    assert(close(sv[1]) == 0);
    assert(close(stats_pipe[1]) == 0);

    if (close_early) {
        assert(close(sv[0]) == 0);
    } else {
        for (;;) {
            struct msg m;
            int rc = sock_recv(sv[0], &m);
            if (rc == SOCK_DISCONNECTED)
                break;
            assert(rc == 0);
            assert(out->n < MAX_MSGS);
            out->msgs[out->n].m = m;
            out->msgs[out->n].arrival_ns = mono_now();
            out->n++;
        }
        assert(close(sv[0]) == 0);
    }

    int status;
    assert(waitpid(pid, &status, 0) == pid);
    out->elapsed_ns = mono_now() - start;
    assert(WIFEXITED(status));
    out->exit_code = WEXITSTATUS(status);
    ssize_t n = read(stats_pipe[0], &out->stats, sizeof out->stats);
    assert(n == (ssize_t)sizeof out->stats || out->exit_code == CHILD_ERROR);
    assert(close(stats_pipe[0]) == 0);
}

/* Collect the readings of a run, in order. Returns how many. */
static int readings(const struct run *r, const struct msg_reading **out, int max)
{
    int n = 0;
    for (int i = 0; i < r->n; i++)
        if (r->msgs[i].m.type == MSG_READING) {
            assert(n < max);
            out[n++] = &r->msgs[i].m.u.reading;
        }
    return n;
}

static void reset_test_backend(void)
{
    slow_read_ns = 0;
    stall_on_read = -1;
    stall_ns = 0;
}

/* ---- tests ------------------------------------------------------------- */

static struct run run; /* large; keep off the stack */

/* 1. Every reading is stamped: seq +1, sensor_id, flags, both clocks. */
static void test_stamps(void)
{
    const struct sensor_loop_cfg cfg = {
        .kind = SENSOR_SOIL, .period_ns = 20 * MS, .heartbeat_ns = 1000 * MS, .count = 10,
    };
    run_loop(&run, &cfg, NULL, 0, 0);
    assert(run.exit_code == CHILD_OK);
    assert(run.stats.readings == 10);
    assert(run.stats.missed_ticks == 0);

    const struct msg_reading *rd[64];
    int n = readings(&run, rd, 64);
    assert(n == 10);
    for (int i = 0; i < n; i++) {
        assert(rd[i]->seq == (uint32_t)i);
        assert(rd[i]->sensor_id == SENSOR_SOIL);
        assert(rd[i]->flags == MSG_FLAG_SIMULATED);
        assert(rd[i]->timestamp_ns > 1600000000LL * 1000000000LL); /* after 2020 */
        assert(rd[i]->mono_ns > 0);
        if (i > 0) {
            assert(rd[i]->mono_ns > rd[i - 1]->mono_ns);
            assert(rd[i]->timestamp_ns >= rd[i - 1]->timestamp_ns);
        }
        assert(rd[i]->value >= 0.0 && rd[i]->value <= 100.0);
    }
}

/*
 * 2. Drift-free: each read takes 2 ms on a 10 ms period. A loop that slept a
 * relative 10 ms after each sample would fall 2 ms further behind every
 * reading (60 ms after 30); the absolute schedule stays on the grid.
 */
static void test_drift_free(void)
{
    reset_test_backend();
    slow_read_ns = 2 * MS;
    const int64_t period = 10 * MS;
    const struct sensor_loop_cfg cfg = {
        .kind = SENSOR_LIGHT, .period_ns = period, .heartbeat_ns = 1000 * MS, .count = 31,
    };
    run_loop(&run, &cfg, &test_ops, 0, 0);
    assert(run.exit_code == CHILD_OK);

    const struct msg_reading *rd[64];
    int n = readings(&run, rd, 64);
    assert(n == 31);
    assert(run.stats.missed_ticks == 0); /* 2 ms of work per 10 ms is no overrun */
    /* Schedule error e_i = mono_i - i*period, relative to the unknown start.
     * On a fixed grid it stays in a narrow band (reading 0 may lag a little,
     * e.g. under valgrind); a drifting loop grows it by 2 ms per reading. */
    int64_t emin = rd[0]->mono_ns, emax = rd[0]->mono_ns;
    for (int i = 1; i < n; i++) {
        int64_t e = rd[i]->mono_ns - (int64_t)i * period;
        if (e < emin) emin = e;
        if (e > emax) emax = e;
    }
    assert(emax - emin < 30 * MS); /* a drifting loop reaches 60 ms */
    /* And the end of the run is no later on the grid than the start. */
    assert((rd[n - 1]->mono_ns - (int64_t)(n - 1) * period) - emin < 30 * MS);
}

/* 3. Heartbeats run on their own grid, independent of the sample period. */
static void test_heartbeats(void)
{
    const int64_t beat = 30 * MS;
    const struct sensor_loop_cfg cfg = {
        .kind = SENSOR_LIGHT, .period_ns = 100 * MS, .heartbeat_ns = beat, .count = 4,
    };
    run_loop(&run, &cfg, NULL, 0, 0);
    assert(run.exit_code == CHILD_OK);

    /* First message is a heartbeat, sent before any reading. */
    assert(run.n > 0);
    assert(run.msgs[0].m.type == MSG_HEARTBEAT);
    assert(run.msgs[0].m.u.heartbeat.reading_count == 0);

    uint32_t readings_seen = 0;
    int beats = 0, beats_between_r0_r1 = 0;
    int64_t prev_beat = 0;
    for (int i = 0; i < run.n; i++) {
        const struct msg *m = &run.msgs[i].m;
        if (m->type == MSG_READING) {
            readings_seen++;
            continue;
        }
        assert(m->type == MSG_HEARTBEAT);
        assert(m->u.heartbeat.pid == (int32_t)run.pid);
        assert(m->u.heartbeat.sensor_id == SENSOR_LIGHT);
        /* Counts exactly the readings already sent (same ordered socket). */
        assert(m->u.heartbeat.reading_count == readings_seen);
        if (readings_seen == 1)
            beats_between_r0_r1++;
        if (beats > 0) {
            int64_t gap = run.msgs[i].arrival_ns - prev_beat;
            assert(gap > beat / 2);
            assert(gap < beat + TOLERANCE);
        }
        prev_beat = run.msgs[i].arrival_ns;
        beats++;
    }
    assert(readings_seen == 4);
    assert((uint32_t)beats == run.stats.heartbeats);
    /* ~300 ms run, one beat per 30 ms plus the first: ~11. */
    assert(beats >= 7 && beats <= 12);
    assert(beats_between_r0_r1 >= 2); /* 100 ms apart, beats every 30 ms */
}

/* Distance from mono to the nearest point on the grid anchored at ref. */
static int64_t grid_error(int64_t mono, int64_t ref, int64_t period)
{
    int64_t r = (mono - ref) % period;
    if (r < 0)
        r += period;
    return r < period - r ? r : period - r;
}

/*
 * 4. An overrun skips the missed ticks: no catch-up burst, back on the grid.
 * The stall ends half-way between ticks, so a loop that re-anchored its
 * schedule there would sit half a period off the original grid.
 */
static void test_overrun(void)
{
    reset_test_backend();
    const int64_t period = 50 * MS;
    stall_on_read = 2;   /* third reading, due at t0 + 100 ms ... */
    stall_ns = 175 * MS; /* ... finishes ~275 ms: ticks 3, 4, 5 are gone */
    const struct sensor_loop_cfg cfg = {
        .kind = SENSOR_SOIL, .period_ns = period, .heartbeat_ns = 1000 * MS, .count = 7,
    };
    run_loop(&run, &cfg, &test_ops, 0, 0);
    assert(run.exit_code == CHILD_OK);
    assert(run.stats.missed_ticks >= 3); /* skipped, not replayed */

    const struct msg_reading *rd[64];
    int n = readings(&run, rd, 64);
    assert(n == 7);
    for (int i = 0; i < n; i++)
        assert(rd[i]->seq == (uint32_t)i); /* seq has no gaps */

    /* No burst: a catch-up loop fires 3, 4 and 5 within a millisecond or so.
     * Any three consecutive readings here span more than a period, even if
     * one is stamped late (it can sit close to its successor, not to two). */
    for (int i = 0; i + 2 < n; i++)
        assert(rd[i + 2]->mono_ns - rd[i]->mono_ns > period);

    /* Back on the original grid. Reading 1 anchors it (reading 0 may lag
     * under valgrind); 4.. are past the stall and its log line. */
    const int64_t ref = rd[1]->mono_ns;
    for (int i = 4; i < n; i++)
        assert(grid_error(rd[i]->mono_ns, ref, period) < 15 * MS);
}

/* 5. Setting *stop from a signal ends the loop promptly, even mid-period. */
static void test_stop(void)
{
    const struct sensor_loop_cfg cfg = {
        .kind = SENSOR_SOIL, .period_ns = 10000 * MS, .heartbeat_ns = 2000 * MS, .count = 0,
    };
    run_loop(&run, &cfg, NULL, 50 * MS, 0);
    assert(run.exit_code == CHILD_OK);
    assert(run.stats.readings == 1);   /* only the one at t0 */
    assert(run.stats.heartbeats == 1); /* only the one at start */
    assert(run.elapsed_ns < 50 * MS + 4 * TOLERANCE); /* not the 2 s / 10 s sleep */
}

/* 6. A vanished peer is reported, not fatal to the caller. */
static void test_disconnect(void)
{
    const struct sensor_loop_cfg cfg = {
        .kind = SENSOR_LIGHT, .period_ns = 10 * MS, .heartbeat_ns = 10 * MS, .count = 0,
    };
    run_loop(&run, &cfg, NULL, 0, 1);
    assert(run.exit_code == CHILD_DISCONNECTED);
}

/* 7. Invalid configuration is rejected before anything is sent. */
static void test_bad_cfg(void)
{
    struct backend b = { 0 };
    struct sensor_loop_stats st;
    struct sensor_loop_cfg cfg = {
        .kind = SENSOR_LIGHT, .period_ns = 0, .heartbeat_ns = 10 * MS,
    };
    assert(sensor_loop(&cfg, &b, -1, &st) == -EINVAL);
    cfg.period_ns = 10 * MS;
    cfg.heartbeat_ns = -1;
    assert(sensor_loop(&cfg, &b, -1, &st) == -EINVAL);
    cfg.heartbeat_ns = 10 * MS;
    cfg.kind = (enum sensor_kind)7;
    assert(sensor_loop(&cfg, &b, -1, &st) == -EINVAL);
    assert(st.readings == 0 && st.heartbeats == 0);
}

int main(void)
{
    test_bad_cfg();
    test_stamps();
    test_drift_free();
    test_heartbeats();
    test_overrun();
    test_stop();
    test_disconnect();
    printf("test_sensor_loop: all asserts passed\n");
    return 0;
}
