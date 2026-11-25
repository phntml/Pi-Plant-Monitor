#define _GNU_SOURCE /* getopt_long */

#include <getopt.h>
#include <stdio.h>
#include <string.h>

#include "backend.h"
#include "log.h"

/*
 * Sensor process.
 *
 * Usage: sensor [--backend=real|fake]
 *
 * Exit codes:
 *   0  clean exit
 *   1  runtime failure (e.g. backend could not be initialized)
 *   2  usage error (unknown option or bad --backend value)
 */

#define EXIT_OK      0
#define EXIT_RUNTIME 1
#define EXIT_USAGE   2

static void usage(FILE *out)
{
    fprintf(out, "usage: sensor [--backend=%s]  (default: %s)\n",
            backend_names(), BACKEND_DEFAULT);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "backend", required_argument, NULL, 'b' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    const char *backend_name = BACKEND_DEFAULT;

    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'b':
            backend_name = optarg;
            break;
        case 'h':
            usage(stdout);
            return EXIT_OK;
        default: /* getopt_long already printed the reason */
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "sensor: unexpected argument '%s'\n", argv[optind]);
        usage(stderr);
        return EXIT_USAGE;
    }
    if (backend_find(backend_name) == NULL) {
        fprintf(stderr, "sensor: invalid --backend value '%s' (expected %s)\n",
                backend_name, backend_names());
        return EXIT_USAGE;
    }

    if (log_open(NULL, "sensor") < 0)
        return EXIT_RUNTIME;

    struct backend b;
    int rc = backend_init(&b, backend_name);
    if (rc < 0) {
        log_error(EV_SENSOR_ERROR, "op=init backend=%s err=%s",
                  backend_name, strerror(-rc));
        return EXIT_RUNTIME;
    }
    log_info(EV_START, "backend=%s", backend_name);

    /* The read loop (backend_read -> struct msg -> hub) goes here once the
     * hub socket exists. */

    rc = backend_close(&b);
    if (rc < 0) {
        log_error(EV_SENSOR_ERROR, "op=close backend=%s err=%s",
                  backend_name, strerror(-rc));
        return EXIT_RUNTIME;
    }
    log_info(EV_STOP, NULL);
    return EXIT_OK;
}
