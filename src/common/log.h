#ifndef LOG_H
#define LOG_H

#include <stddef.h> /* NULL, for callers passing a NULL path or fmt */

/*
 * Event logger shared by every daemon.
 *
 * Each event is one line:
 *
 *   <timestamp> <LEVEL> <name>[<pid>] <EVENT> <message>
 *   2026-11-24T18:03:07.412Z WARN hub[4121] DISCONNECT sensor_id=2 fd=7
 *
 *   timestamp  CLOCK_REALTIME in UTC, ISO 8601, millisecond precision
 *   LEVEL      INFO, WARN or ERROR
 *   name       process name given to log_open()
 *   pid        getpid() at the time of the event
 *   EVENT      one of the fixed names in LOG_EVENTS below
 *   message    optional free text, by convention key=value pairs;
 *              omitted (with its leading space) when empty
 *
 * Fields are separated by single spaces, so scripts can split on ' '
 * for the first five fields and treat the rest as the message.
 *
 * Atomicity: the whole line is formatted into a buffer and written with a
 * single write(). The log file is opened O_APPEND, so lines from several
 * processes appending to the same file never interleave or overwrite each
 * other. Lines are capped at LOG_LINE_MAX bytes (no larger than
 * _POSIX_PIPE_BUF) so the same holds when logging to stderr through a pipe.
 * A longer message is truncated and ends in "...". Control characters in
 * the message (including newlines) are replaced with spaces, so one event
 * is always exactly one line.
 *
 * Not async-signal-safe (uses vsnprintf); do not call from signal handlers.
 */

#define LOG_LINE_MAX 512 /* bytes per line, including the '\n' */
#define LOG_NAME_MAX 31  /* longer process names are truncated */

enum log_level {
    LOG_LEVEL_INFO  = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_ERROR = 2,
};

/*
 * Fixed event names. Soak-test and fault-injection scripts grep for these,
 * so never rename or reuse one; only add new ones. EV_FOO logs as "FOO".
 */
#define LOG_EVENTS(X)                                                          \
    X(START)            /* daemon started; first line it logs */              \
    X(STOP)             /* daemon exiting cleanly; last line it logs */       \
    X(HEARTBEAT)        /* periodic liveness record */                        \
    X(HEARTBEAT_MISSED) /* a peer's heartbeat is overdue */                   \
    X(CHILD_SPAWN)      /* supervisor started a child */                      \
    X(CHILD_EXIT)       /* supervisor reaped a child (exit status / signal) */\
    X(CHILD_RESTART)    /* supervisor is restarting a child */                \
    X(CONNECT)          /* peer connected to a socket */                      \
    X(DISCONNECT)       /* peer closed or lost its connection */              \
    X(BAD_MSG)          /* malformed or unexpected message received */        \
    X(SENSOR_ERROR)     /* sensor read failed */                              \
    X(OUT_OF_RANGE)     /* reading outside the sensor's valid range */        \
    X(STORE_FLUSH)      /* buffered readings written to storage */            \
    X(STORE_ERROR)      /* storage write failed */                            \
    X(SIGNAL)           /* signal received (e.g. SIGTERM, SIGHUP) */          \
    X(SYSCALL_FAIL)     /* a syscall failed; message names call and errno */

#define LOG_EVENT_ENUM_(name) EV_##name,
enum log_ev {
    LOG_EVENTS(LOG_EVENT_ENUM_)
    EV_COUNT
};
#undef LOG_EVENT_ENUM_

/*
 * Open (or create, mode 0644) the log file at path for appending, and set
 * the process name used on every line. path == NULL logs to stderr.
 * name must be non-empty and contain no whitespace or control characters.
 * Calling it again replaces the previous file and name.
 * The descriptor is close-on-exec. Returns 0 or -errno.
 */
int log_open(const char *path, const char *name);

/* Close the log file; later events go to stderr. Returns 0 or -errno. */
int log_close(void);

/*
 * Log one event. fmt may be NULL for no message. Before log_open(), lines
 * go to stderr with the name "?". If writing to the log file fails, the
 * line is written to stderr instead. Returns 0 or -errno.
 */
int log_event(enum log_level level, enum log_ev ev, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define log_info(ev, ...)  log_event(LOG_LEVEL_INFO, (ev), __VA_ARGS__)
#define log_warn(ev, ...)  log_event(LOG_LEVEL_WARN, (ev), __VA_ARGS__)
#define log_error(ev, ...) log_event(LOG_LEVEL_ERROR, (ev), __VA_ARGS__)

/* "START" for EV_START, etc.; NULL if ev is out of range. */
const char *log_event_name(enum log_ev ev);

#endif /* LOG_H */
