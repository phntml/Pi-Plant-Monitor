#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static_assert(LOG_LINE_MAX <= _POSIX_PIPE_BUF, "log line must fit in one atomic pipe write");

static int  log_fd = -1; /* -1: log to stderr */
static char log_name[LOG_NAME_MAX + 1] = "?";

static const char *const level_names[] = {
    [LOG_LEVEL_INFO]  = "INFO",
    [LOG_LEVEL_WARN]  = "WARN",
    [LOG_LEVEL_ERROR] = "ERROR",
};

#define LOG_EVENT_NAME_(name) [EV_##name] = #name,
static const char *const event_names[EV_COUNT] = {
    LOG_EVENTS(LOG_EVENT_NAME_)
};
#undef LOG_EVENT_NAME_

const char *log_event_name(enum log_ev ev)
{
    if ((unsigned)ev >= EV_COUNT)
        return NULL;
    return event_names[ev];
}

/* One write() of the whole line; a partial write is never completed, since
 * a second write() could interleave with another process. */
static int write_line(int fd, const char *buf, size_t len)
{
    ssize_t n;
    do {
        n = write(fd, buf, len);
    } while (n < 0 && errno == EINTR);

    if (n < 0)
        return -errno;
    if ((size_t)n != len)
        return -EIO;
    return 0;
}

static void report(const char *call, const char *detail, int err)
{
    char buf[LOG_LINE_MAX];
    int n = snprintf(buf, sizeof buf, "log: %s(%s): %s\n", call, detail, strerror(err));
    if (n > 0)
        (void)write_line(STDERR_FILENO, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static int valid_name(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;
    for (const char *p = name; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ' || c == 0x7f || c == '[' || c == ']')
            return 0;
    }
    return 1;
}

int log_open(const char *path, const char *name)
{
    if (!valid_name(name))
        return -EINVAL;

    int fd = -1;
    if (path != NULL) {
        fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            int err = errno;
            report("open", path, err);
            return -err;
        }
    }

    int rc = log_close();
    log_fd = fd;
    snprintf(log_name, sizeof log_name, "%s", name);
    return rc;
}

int log_close(void)
{
    if (log_fd < 0)
        return 0;

    int fd = log_fd;
    log_fd = -1;
    /* On Linux the fd is released even when close() fails, so never retry. */
    if (close(fd) < 0) {
        int err = errno;
        report("close", "log file", err);
        return -err;
    }
    return 0;
}

int log_event(enum log_level level, enum log_ev ev, const char *fmt, ...)
{
    if ((unsigned)level >= sizeof level_names / sizeof level_names[0] ||
        (unsigned)ev >= EV_COUNT)
        return -EINVAL;

    /* +1 for the NUL that snprintf always writes; it is never sent. */
    char line[LOG_LINE_MAX + 1];
    const size_t body_max = LOG_LINE_MAX - 1; /* leave room for '\n' */

    struct timespec ts;
    struct tm tm;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0 || gmtime_r(&ts.tv_sec, &tm) == NULL) {
        memset(&ts, 0, sizeof ts);
        memset(&tm, 0, sizeof tm);
    }

    int n = snprintf(line, sizeof line,
                     "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %s %s[%ld] %s",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L,
                     level_names[level], log_name, (long)getpid(), event_names[ev]);
    if (n < 0)
        return -EIO;
    size_t len = (size_t)n < body_max ? (size_t)n : body_max;

    if (fmt != NULL && len + 1 < body_max) {
        size_t msg_start = len + 1;
        line[len] = ' ';

        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(line + msg_start, body_max - msg_start + 1, fmt, ap);
        va_end(ap);

        if (m > 0) {
            size_t want = msg_start + (size_t)m;
            if (want > body_max) {
                len = body_max;
                memcpy(line + len - 3, "...", 3);
            } else {
                len = want;
            }
            for (size_t i = msg_start; i < len; i++) {
                unsigned char c = (unsigned char)line[i];
                if (c < ' ' || c == 0x7f)
                    line[i] = ' ';
            }
        }
        /* m == 0 (empty message) or m < 0 (bad format): no message field. */
    }
    line[len++] = '\n';

    if (log_fd < 0)
        return write_line(STDERR_FILENO, line, len);

    int rc = write_line(log_fd, line, len);
    if (rc < 0) {
        /* Don't lose the event: report the failure and the line on stderr. */
        report("write", "log file", -rc);
        (void)write_line(STDERR_FILENO, line, len);
    }
    return rc;
}
