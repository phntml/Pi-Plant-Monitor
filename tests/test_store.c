#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "msg.h"
#include "store.h"

static struct msg_reading reading(uint32_t sensor, uint32_t seq, double v, uint32_t flags)
{
    struct msg_reading r;
    memset(&r, 0, sizeof r);
    r.sensor_id = sensor;
    r.seq = seq;
    r.timestamp_ns = 1795629787412000000LL; /* 2026-11-25T18:03:07.412Z */
    r.mono_ns = 5123456789LL;
    r.value = v;
    r.flags = flags;
    return r;
}

static void expect_row(const struct msg_reading *r, const char *want)
{
    char buf[STORE_ROW_MAX];
    int n = store_format_row(buf, sizeof buf, r);
    assert(n == (int)strlen(want));
    assert(strcmp(buf, want) == 0);
}

static void test_format(void)
{
    struct msg_reading r;

    r = reading(SENSOR_LIGHT, 42, 1234.564, MSG_FLAG_SIMULATED);
    expect_row(&r, "2026-11-25T18:03:07.412Z,5123456789,light,42,1234.56,SYNTHETIC\n");

    r = reading(SENSOR_SOIL, 0, 0, MSG_FLAG_SIMULATED | MSG_FLAG_SENSOR_ERROR);
    expect_row(&r, "2026-11-25T18:03:07.412Z,5123456789,soil,0,0.00,SYNTHETIC|ERROR\n");

    r = reading(SENSOR_SOIL, 7, 101.5,
                MSG_FLAG_OUT_OF_RANGE | MSG_FLAG_SENSOR_ERROR | MSG_FLAG_SIMULATED);
    expect_row(&r, "2026-11-25T18:03:07.412Z,5123456789,soil,7,101.50,SYNTHETIC|ERROR|OUT_OF_RANGE\n");

    /* Column order is fixed: wall_time,mono_ns,sensor,seq,value,flags. */
    assert(strcmp(STORE_HEADER, "wall_time,mono_ns,sensor,seq,value,flags\n") == 0);

    /* mono_ns is printed in full, not truncated or rounded. */
    r = reading(SENSOR_LIGHT, 2, 1.0, MSG_FLAG_SIMULATED);
    r.mono_ns = INT64_C(9223372036854775807);
    expect_row(&r, "2026-11-25T18:03:07.412Z,9223372036854775807,light,2,1.00,SYNTHETIC\n");

    r = reading(SENSOR_LIGHT, 1, 3.0, 0); /* real reading: no flags */
    expect_row(&r, "2026-11-25T18:03:07.412Z,5123456789,light,1,3.00,\n");

    char buf[STORE_ROW_MAX];
    r = reading(0, 0, 0, 0);
    assert(store_format_row(buf, sizeof buf, &r) == -EINVAL);
    r = reading(3, 0, 0, 0);
    assert(store_format_row(buf, sizeof buf, &r) == -EINVAL);

    /* Too small at every possible cut point, including inside the flags. */
    r = reading(SENSOR_SOIL, 7, 1.0, MSG_FLAG_SIMULATED | MSG_FLAG_SENSOR_ERROR);
    int full = store_format_row(buf, sizeof buf, &r);
    assert(full > 0);
    for (size_t size = 0; size <= (size_t)full; size++) {
        char small[STORE_ROW_MAX];
        assert(store_format_row(small, size, &r) == -ENOSPC);
    }
    assert(store_format_row(buf, (size_t)full + 1, &r) == full);
}

static void read_file(const char *path, char *out, size_t size)
{
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    ssize_t n = read(fd, out, size - 1);
    assert(n >= 0);
    out[n] = '\0';
    assert(close(fd) == 0);
}

static void test_open_append(void)
{
    char dir[] = "/tmp/test_store-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char path[64];
    assert(snprintf(path, sizeof path, "%s/r.csv", dir) < (int)sizeof path);

    int fd = store_open(path);
    assert(fd >= 0);
    struct msg_reading r = reading(SENSOR_LIGHT, 0, 5.0, MSG_FLAG_SIMULATED);
    assert(store_append(fd, &r) == 0);
    assert(store_close(fd) == 0);

    /* Reopening an existing file must not write the header again. */
    fd = store_open(path);
    assert(fd >= 0);
    r = reading(SENSOR_SOIL, 0, 6.0, MSG_FLAG_SIMULATED);
    assert(store_append(fd, &r) == 0);
    r = reading(9, 0, 6.0, MSG_FLAG_SIMULATED);
    assert(store_append(fd, &r) == -EINVAL); /* nothing written */
    assert(store_close(fd) == 0);

    char got[512];
    read_file(path, got, sizeof got);
    assert(strcmp(got, STORE_HEADER
                       "2026-11-25T18:03:07.412Z,5123456789,light,0,5.00,SYNTHETIC\n"
                       "2026-11-25T18:03:07.412Z,5123456789,soil,0,6.00,SYNTHETIC\n") == 0);

    assert(store_open("/nonexistent/dir/r.csv") == -ENOENT);

    assert(unlink(path) == 0);
    assert(rmdir(dir) == 0);
}

int main(void)
{
    test_format();
    test_open_append();
    printf("test_store: all asserts passed\n");
    return 0;
}
