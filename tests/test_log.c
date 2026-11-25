#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"

#define WRITERS     8
#define PER_WRITER  2000

static const char *line_re =
    "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}Z "
    "(INFO|WARN|ERROR) [^][ ]+\\[[0-9]+\\] [A-Z_]+( [^\n]*)?\n$";

/* Read the whole file into a NUL-terminated heap buffer. */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size >= 0);
    rewind(f);
    char *buf = malloc((size_t)size + 1);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)size, f) == (size_t)size);
    buf[size] = '\0';
    assert(fclose(f) == 0);
    *len = (size_t)size;
    return buf;
}

/* Split buf into lines (keeping '\n'), check each against the format. */
static size_t check_lines(char *buf, size_t len, const regex_t *re,
                          char **lines, size_t max_lines)
{
    size_t count = 0;
    char *p = buf;
    while (p < buf + len) {
        char *nl = strchr(p, '\n');
        assert(nl != NULL); /* file ends with a complete line */
        char saved = nl[1];
        nl[1] = '\0';
        assert((size_t)(nl - p + 1) <= LOG_LINE_MAX);
        if (regexec(re, p, 0, NULL, 0) != 0) {
            fprintf(stderr, "bad line: %s", p);
            assert(0);
        }
        nl[1] = saved;
        assert(count < max_lines);
        lines[count++] = p;
        p = nl + 1;
    }
    return count;
}

static void test_names(void)
{
    for (int i = 0; i < EV_COUNT; i++) {
        const char *a = log_event_name((enum log_ev)i);
        assert(a != NULL && a[0] != '\0');
        for (const char *c = a; *c; c++)
            assert((*c >= 'A' && *c <= 'Z') || *c == '_');
        for (int j = 0; j < i; j++)
            assert(strcmp(a, log_event_name((enum log_ev)j)) != 0);
    }
    assert(strcmp(log_event_name(EV_START), "START") == 0);
    assert(strcmp(log_event_name(EV_SYSCALL_FAIL), "SYSCALL_FAIL") == 0);
    assert(log_event_name(EV_COUNT) == NULL);
}

static void test_format(const char *path, const regex_t *re)
{
    assert(unlink(path) == 0 || errno == ENOENT);
    assert(log_open(path, "testproc") == 0);

    assert(log_info(EV_START, "version=%d", 1) == 0);
    assert(log_warn(EV_DISCONNECT, NULL) == 0);
    assert(log_error(EV_SYSCALL_FAIL, "call=%s err=%s", "read", "EIO") == 0);
    assert(log_info(EV_HEARTBEAT, "a\nb\tc") == 0); /* must stay one line */
    char big[2 * LOG_LINE_MAX];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    assert(log_info(EV_STORE_FLUSH, "%s", big) == 0);

    /* Bad arguments are rejected without writing. */
    assert(log_event((enum log_level)3, EV_START, NULL) == -EINVAL);
    assert(log_event(LOG_LEVEL_INFO, EV_COUNT, NULL) == -EINVAL);
    assert(log_close() == 0);

    size_t len;
    char *buf = slurp(path, &len);
    char *lines[8];
    assert(check_lines(buf, len, re, lines, 8) == 5);

    char pid[32];
    snprintf(pid, sizeof pid, "testproc[%ld] ", (long)getpid());
    for (int i = 0; i < 5; i++)
        assert(strstr(lines[i], pid) != NULL);

    assert(strstr(lines[0], " INFO testproc[") != NULL);
    assert(strstr(lines[0], "] START version=1\n") != NULL);
    assert(strstr(lines[1], " WARN testproc[") != NULL);
    assert(strstr(lines[1], "] DISCONNECT\n") != NULL);
    assert(strstr(lines[2], " ERROR testproc[") != NULL);
    assert(strstr(lines[2], "] SYSCALL_FAIL call=read err=EIO\n") != NULL);
    assert(strstr(lines[3], "] HEARTBEAT a b c\n") != NULL);
    /* Truncated to exactly LOG_LINE_MAX bytes, ending in "...". */
    assert(lines[4] + LOG_LINE_MAX == buf + len);
    assert(memcmp(buf + len - 4, "...\n", 4) == 0);

    free(buf);
}

static void test_errors(const char *path)
{
    assert(log_open(path, NULL) == -EINVAL);
    assert(log_open(path, "") == -EINVAL);
    assert(log_open(path, "has space") == -EINVAL);
    assert(log_open(path, "br[ack]et") == -EINVAL);
    assert(log_open("/nonexistent-dir/x.log", "t") == -ENOENT);

    /* Log fd must not leak into exec'd children. open() returns the lowest
     * free descriptor, so the log gets the one we just released. */
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(log_open(path, "t") == 0);
    int flags = fcntl(fd, F_GETFD);
    assert(flags >= 0 && (flags & FD_CLOEXEC));
    assert(log_close() == 0);
    assert(log_close() == 0); /* idempotent */
}

/* Several processes appending to one file never interleave lines. */
static void test_concurrent(const char *path, const regex_t *re)
{
    assert(unlink(path) == 0 || errno == ENOENT);

    pid_t kids[WRITERS];
    for (int w = 0; w < WRITERS; w++) {
        kids[w] = fork();
        assert(kids[w] >= 0);
        if (kids[w] == 0) {
            char name[16];
            snprintf(name, sizeof name, "writer%d", w);
            if (log_open(path, name) != 0)
                _exit(1);
            char pad[400];
            memset(pad, 'a' + w, sizeof pad - 1);
            pad[sizeof pad - 1] = '\0';
            for (int i = 0; i < PER_WRITER; i++)
                if (log_info(EV_HEARTBEAT, "w=%d seq=%d pad=%s", w, i, pad) != 0)
                    _exit(1);
            _exit(log_close() == 0 ? 0 : 1);
        }
    }
    for (int w = 0; w < WRITERS; w++) {
        int status;
        assert(waitpid(kids[w], &status, 0) == kids[w]);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    size_t len;
    char *buf = slurp(path, &len);
    size_t max = WRITERS * PER_WRITER + 1;
    char **lines = malloc(max * sizeof *lines);
    assert(lines != NULL);
    assert(check_lines(buf, len, re, lines, max) == WRITERS * PER_WRITER);

    /* Each writer's lines are intact and in order. */
    int next[WRITERS] = { 0 };
    for (size_t i = 0; i < WRITERS * PER_WRITER; i++) {
        int w, seq, off = 0;
        char *msg = strstr(lines[i], "] HEARTBEAT ");
        assert(msg != NULL);
        assert(sscanf(msg, "] HEARTBEAT w=%d seq=%d pad=%n", &w, &seq, &off) == 2);
        assert(off > 0 && w >= 0 && w < WRITERS && seq == next[w]);
        next[w]++;
        for (int k = 0; k < 399; k++)
            assert(msg[off + k] == 'a' + w);
        assert(msg[off + 399] == '\n');
    }
    for (int w = 0; w < WRITERS; w++)
        assert(next[w] == PER_WRITER);

    free(lines);
    free(buf);
}

int main(void)
{
    char path[64];
    snprintf(path, sizeof path, "/tmp/test_log.%ld", (long)getpid());

    regex_t re;
    assert(regcomp(&re, line_re, REG_EXTENDED | REG_NOSUB) == 0);

    test_names();
    test_format(path, &re);
    test_errors(path);
    test_concurrent(path, &re);

    regfree(&re);
    assert(unlink(path) == 0);
    printf("test_log: all asserts passed\n");
    return 0;
}
