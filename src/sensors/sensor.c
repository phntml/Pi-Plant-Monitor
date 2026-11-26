#define _GNU_SOURCE /* getopt_long */

#include "sensor.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "sensor_loop.h"
#include "sock.h"

#define EXIT_OK      0
#define EXIT_RUNTIME 1
#define EXIT_USAGE   2

#define PERIOD_MS_LIGHT 1000UL
#define PERIOD_MS_SOIL  10000UL
#define HEARTBEAT_MS    2000UL /* design doc: heartbeats every 2 s */
#define MS_MAX          (24UL * 3600 * 1000)

static volatile sig_atomic_t stop_requested;

static void on_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static const char *proc_name(enum sensor_kind kind)
{
    return kind == SENSOR_LIGHT ? "light_proc" : "soil_proc";
}

static void usage(FILE *out, enum sensor_kind kind)
{
    fprintf(out,
            "usage: %s [--backend=%s] [--socket=PATH] [--count=N] [--period-ms=N]\n"
            "          [--heartbeat-ms=N]\n"
            "  defaults: --backend=%s --socket=%s --count=0 (forever) --period-ms=%lu\n"
            "            --heartbeat-ms=%lu\n",
            proc_name(kind), backend_names(), BACKEND_DEFAULT, SENSOR_SOCKET_DEFAULT,
            kind == SENSOR_LIGHT ? PERIOD_MS_LIGHT : PERIOD_MS_SOIL, HEARTBEAT_MS);
}

/* Parse a non-negative decimal integer no larger than max. */
static int parse_ulong(const char *s, unsigned long max, unsigned long *out)
{
    if (s[0] < '0' || s[0] > '9')
        return -1;
    errno = 0;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || *end != '\0' || v > max)
        return -1;
    *out = v;
    return 0;
}

static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop; /* no SA_RESTART: interrupt the sleep */
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) < 0 || sigaction(SIGINT, &sa, NULL) < 0)
        return -errno;
    return 0;
}

int sensor_main(enum sensor_kind kind, int argc, char **argv)
{
    static const struct option opts[] = {
        { "backend",   required_argument, NULL, 'b' },
        { "socket",    required_argument, NULL, 's' },
        { "count",     required_argument, NULL, 'c' },
        { "period-ms", required_argument, NULL, 'p' },
        { "heartbeat-ms", required_argument, NULL, 'H' },
        { "help",      no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    const char *name = proc_name(kind);
    const char *backend_name = BACKEND_DEFAULT;
    const char *socket_path = SENSOR_SOCKET_DEFAULT;
    unsigned long count = 0;
    unsigned long period_ms = kind == SENSOR_LIGHT ? PERIOD_MS_LIGHT : PERIOD_MS_SOIL;
    unsigned long heartbeat_ms = HEARTBEAT_MS;

    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'b':
            backend_name = optarg;
            break;
        case 's':
            socket_path = optarg;
            break;
        case 'c':
            if (parse_ulong(optarg, UINT32_MAX, &count) < 0) {
                fprintf(stderr, "%s: invalid --count value '%s'\n", name, optarg);
                return EXIT_USAGE;
            }
            break;
        case 'p':
            if (parse_ulong(optarg, MS_MAX, &period_ms) < 0 || period_ms == 0) {
                fprintf(stderr, "%s: invalid --period-ms value '%s'\n", name, optarg);
                return EXIT_USAGE;
            }
            break;
        case 'H':
            if (parse_ulong(optarg, MS_MAX, &heartbeat_ms) < 0 || heartbeat_ms == 0) {
                fprintf(stderr, "%s: invalid --heartbeat-ms value '%s'\n", name, optarg);
                return EXIT_USAGE;
            }
            break;
        case 'h':
            usage(stdout, kind);
            return EXIT_OK;
        default: /* getopt_long already printed the reason */
            usage(stderr, kind);
            return EXIT_USAGE;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "%s: unexpected argument '%s'\n", name, argv[optind]);
        usage(stderr, kind);
        return EXIT_USAGE;
    }
    if (backend_find(backend_name) == NULL) {
        fprintf(stderr, "%s: invalid --backend value '%s' (expected %s)\n",
                name, backend_name, backend_names());
        return EXIT_USAGE;
    }

    if (log_open(NULL, name) < 0)
        return EXIT_RUNTIME;

    int exit_code = EXIT_RUNTIME;
    int fd = -1;
    struct sensor_loop_stats stats = { 0 };
    struct backend b;
    int rc = backend_init(&b, backend_name, kind);
    if (rc < 0) {
        log_error(EV_SENSOR_ERROR, "op=init backend=%s err=%s",
                  backend_name, strerror(-rc));
        return EXIT_RUNTIME;
    }

    rc = install_signals();
    if (rc < 0) {
        log_error(EV_SYSCALL_FAIL, "op=sigaction err=%s", strerror(-rc));
        goto cleanup;
    }

    fd = sock_connect(socket_path);
    if (fd < 0) {
        log_error(EV_SYSCALL_FAIL, "op=connect socket=%s err=%s",
                  socket_path, strerror(-fd));
        goto cleanup;
    }
    log_info(EV_START, "backend=%s sensor=%s period_ms=%lu heartbeat_ms=%lu",
             backend_name, sensor_kind_name(kind), period_ms, heartbeat_ms);

    const struct sensor_loop_cfg cfg = {
        .kind         = kind,
        .period_ns    = (int64_t)period_ms * 1000000LL,
        .heartbeat_ns = (int64_t)heartbeat_ms * 1000000LL,
        .count        = (uint32_t)count,
        .stop         = &stop_requested,
    };
    rc = sensor_loop(&cfg, &b, fd, &stats);
    if (rc == SOCK_DISCONNECTED) {
        log_error(EV_DISCONNECT, "socket=%s readings=%u", socket_path, stats.readings);
        goto cleanup;
    }
    if (rc < 0) {
        log_error(EV_SYSCALL_FAIL, "op=sensor_loop readings=%u err=%s",
                  stats.readings, strerror(-rc));
        goto cleanup;
    }
    if (stop_requested)
        log_info(EV_SIGNAL, "stopping readings=%u", stats.readings);
    exit_code = EXIT_OK;

cleanup:
    if (fd >= 0 && sock_close(fd) < 0)
        exit_code = EXIT_RUNTIME;
    rc = backend_close(&b);
    if (rc < 0) {
        log_error(EV_SENSOR_ERROR, "op=close backend=%s err=%s",
                  backend_name, strerror(-rc));
        exit_code = EXIT_RUNTIME;
    }
    if (exit_code == EXIT_OK)
        log_info(EV_STOP, "readings=%u heartbeats=%u missed=%llu", stats.readings,
                 stats.heartbeats, (unsigned long long)stats.missed_ticks);
    return exit_code;
}
