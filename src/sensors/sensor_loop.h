#ifndef SENSOR_LOOP_H
#define SENSOR_LOOP_H

#include <signal.h>
#include <stdint.h>

#include "backend.h"

/*
 * The sampling loop shared by every sensor process.
 *
 * Timing is drift-free: t0 = CLOCK_MONOTONIC at start, sample k is due at
 * t0 + k*period_ns and heartbeat j at t0 + j*heartbeat_ns. Deadlines are
 * computed from k and j, never from the previous wake-up, and the loop
 * sleeps with clock_nanosleep(TIMER_ABSTIME) until the earlier of the two,
 * so per-iteration overhead never accumulates.
 *
 *   readings    seq = 0, 1, 2, ... (+1 per reading sent), sensor_id,
 *               timestamp_ns (CLOCK_REALTIME) and mono_ns (CLOCK_MONOTONIC)
 *               read back to back at sample time; MSG_FLAG_SIMULATED when the
 *               backend is simulated, MSG_FLAG_SENSOR_ERROR on a failed read.
 *   heartbeats  one immediately at start, then on their own grid,
 *               independent of the sample period and of overruns.
 *   overrun     if a sample runs past later ticks, those ticks are skipped
 *               (logged once as SAMPLE_OVERRUN, counted in missed_ticks) and
 *               the loop resumes on the original grid: no catch-up burst.
 *               Skipped ticks show as a gap in mono_ns, not in seq.
 *               Late heartbeats are likewise sent once, not in a burst.
 *
 * Stopping: the loop returns once *stop is nonzero. A signal handler that
 * sets it interrupts the sleep; one that arrives just before the sleep
 * starts is seen at the next wake-up, at most min(period, heartbeat) later.
 */

struct sensor_loop_cfg {
    enum sensor_kind kind;
    int64_t  period_ns;           /* sample period, > 0 */
    int64_t  heartbeat_ns;        /* heartbeat interval, > 0 */
    uint32_t count;               /* stop after this many readings; 0 = no limit */
    volatile sig_atomic_t *stop;  /* may be NULL */
};

struct sensor_loop_stats {
    uint32_t readings;     /* readings sent */
    uint32_t heartbeats;   /* heartbeats sent */
    uint64_t missed_ticks; /* sample ticks skipped by overruns */
};

/*
 * Run the loop on an initialized backend, sending to the connected socket fd.
 * st is always filled in. Returns 0 on a clean stop (count reached or *stop
 * set), SOCK_DISCONNECTED if the peer went away, or -errno (-EINVAL for a
 * bad cfg).
 */
int sensor_loop(const struct sensor_loop_cfg *cfg, struct backend *b, int fd,
                struct sensor_loop_stats *st);

#endif /* SENSOR_LOOP_H */
