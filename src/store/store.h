#ifndef STORE_H
#define STORE_H

#include <stddef.h>

#include "msg.h"

/*
 * Minimal reading store: one CSV row per reading, appended with a single
 * write() to a file opened O_APPEND.
 *
 *   wall_time,mono_ns,sensor,seq,value,flags
 *   2026-11-25T18:03:07.412Z,5123456789,light,42,1234.56,SYNTHETIC
 *
 *   wall_time  reading's CLOCK_REALTIME, UTC, ISO 8601, millisecond precision
 *   mono_ns    reading's CLOCK_MONOTONIC in nanoseconds (for intervals,
 *              jitter and gaps; unaffected by wall-clock changes)
 *   sensor     light | soil
 *   seq        per-sensor sequence number
 *   value      two decimals
 *   flags      '|'-separated, always in this order, empty if none:
 *                SYNTHETIC     MSG_FLAG_SIMULATED (no real hardware)
 *                ERROR         MSG_FLAG_SENSOR_ERROR
 *                OUT_OF_RANGE  MSG_FLAG_OUT_OF_RANGE
 *
 * Not yet: batching, fsync policy, daily files, retention, recovery.
 */

#define STORE_HEADER  "wall_time,mono_ns,sensor,seq,value,flags\n"
#define STORE_ROW_MAX 128 /* bytes, including '\n' */

/*
 * Open (creating, mode 0644) the CSV at path for appending; write the header
 * if the file is empty. Close-on-exec. Returns the fd or -errno.
 */
int store_open(const char *path);

/*
 * Format one row, '\n'-terminated and NUL-terminated, into buf.
 * Returns the row length (without the NUL), -EINVAL for an unknown
 * sensor_id, or -ENOSPC if buf is too small.
 */
int store_format_row(char *buf, size_t size, const struct msg_reading *r);

/* Append one row to fd with a single write(). Returns 0 or -errno. */
int store_append(int fd, const struct msg_reading *r);

/* Close fd. Returns 0 or -errno. */
int store_close(int fd);

#endif /* STORE_H */
