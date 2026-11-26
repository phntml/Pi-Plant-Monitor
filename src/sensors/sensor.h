#ifndef SENSOR_H
#define SENSOR_H

#include "backend.h"

/*
 * Shared main for the sensor processes (light_proc, soil_proc).
 *
 * Usage: <proc> [--backend=real|fake] [--socket=PATH] [--count=N]
 *               [--period-ms=N]
 *
 *   --backend    backend to read from (default: real)
 *   --socket     hub socket to send readings to (default: SENSOR_SOCKET_DEFAULT)
 *   --count      stop cleanly after N readings; 0 = run until signalled
 *   --period-ms  sample period (default: 1000 for light, 10000 for soil)
 *
 * Every reading from a simulated backend carries MSG_FLAG_SIMULATED.
 *
 * Exit codes:
 *   0  clean exit (--count reached, or SIGTERM/SIGINT)
 *   1  runtime failure (backend init, hub connect/send, hub disconnected)
 *   2  usage error (unknown option or bad value)
 */

#define SENSOR_SOCKET_DEFAULT "hub.sock"

int sensor_main(enum sensor_kind kind, int argc, char **argv);

#endif /* SENSOR_H */
