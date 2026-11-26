#define _POSIX_C_SOURCE 200809L

#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const struct {
    uint32_t bit;
    const char *name;
} flag_names[] = {
    { MSG_FLAG_SIMULATED,    "SYNTHETIC" },
    { MSG_FLAG_SENSOR_ERROR, "ERROR" },
    { MSG_FLAG_OUT_OF_RANGE, "OUT_OF_RANGE" },
};

static int write_all_once(int fd, const char *buf, size_t len)
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

int store_open(const char *path)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -errno;

    struct stat st;
    int rc;
    if (fstat(fd, &st) < 0) {
        rc = -errno;
        goto fail;
    }
    if (st.st_size == 0) {
        rc = write_all_once(fd, STORE_HEADER, sizeof STORE_HEADER - 1);
        if (rc < 0)
            goto fail;
    }
    return fd;

fail:
    (void)close(fd);
    return rc;
}

int store_format_row(char *buf, size_t size, const struct msg_reading *r)
{
    const char *sensor = sensor_kind_name(r->sensor_id);
    if (sensor == NULL)
        return -EINVAL;

    time_t sec = (time_t)(r->timestamp_ns / 1000000000LL);
    long ms = (long)((r->timestamp_ns % 1000000000LL) / 1000000LL);
    if (ms < 0) { /* before the epoch: keep ms in [0, 999] */
        sec--;
        ms += 1000;
    }
    struct tm tm;
    if (gmtime_r(&sec, &tm) == NULL)
        memset(&tm, 0, sizeof tm);

    int n = snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ,%lld,%s,%u,%.2f,",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
                     (long long)r->mono_ns, sensor, r->seq, r->value);
    if (n < 0)
        return -EIO;
    size_t len = (size_t)n;
    if (len >= size)
        return -ENOSPC;

    int first = 1;
    for (size_t i = 0; i < sizeof flag_names / sizeof flag_names[0]; i++) {
        if (!(r->flags & flag_names[i].bit))
            continue;
        n = snprintf(buf + len, size - len, "%s%s", first ? "" : "|", flag_names[i].name);
        if (n < 0)
            return -EIO;
        len += (size_t)n;
        if (len >= size)
            return -ENOSPC;
        first = 0;
    }
    if (len + 2 > size) /* '\n' and NUL */
        return -ENOSPC;
    buf[len++] = '\n';
    buf[len] = '\0';
    return (int)len;
}

int store_append(int fd, const struct msg_reading *r)
{
    char row[STORE_ROW_MAX];
    int n = store_format_row(row, sizeof row, r);
    if (n < 0)
        return n;
    return write_all_once(fd, row, (size_t)n);
}

int store_close(int fd)
{
    /* On Linux the fd is released even when close() fails, so never retry. */
    if (close(fd) < 0)
        return -errno;
    return 0;
}
