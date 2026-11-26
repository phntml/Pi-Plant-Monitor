#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backend.h"

static void test_find(void)
{
    const struct backend_ops *real = backend_find("real");
    const struct backend_ops *fake = backend_find("fake");
    assert(real != NULL && strcmp(real->name, "real") == 0 && !real->simulated);
    assert(fake != NULL && strcmp(fake->name, "fake") == 0 && fake->simulated);
    assert(backend_find(NULL) == real); /* default is real */
    assert(strcmp(BACKEND_DEFAULT, "real") == 0);
    assert(backend_find("bogus") == NULL);
    assert(backend_find("") == NULL);
    assert(backend_find("FAKE") == NULL);
}

/* Read n values from a fresh fake backend of the given kind. */
static void read_fake(enum sensor_kind kind, double *out, int n)
{
    struct backend b;
    assert(backend_init(&b, "fake", kind) == 0);
    assert(b.ops == backend_find("fake"));
    for (int i = 0; i < n; i++) {
        struct backend_reading r = backend_read(&b);
        assert(r.status == BACKEND_OK);
        assert(r.err == 0);
        out[i] = r.value;
    }
    assert(backend_close(&b) == 0);
    assert(b.ops == NULL);
    assert(backend_close(&b) == 0); /* idempotent */

    /* A closed backend reports a failed reading, not a value. */
    struct backend_reading r = backend_read(&b);
    assert(r.status == BACKEND_FAIL);
    assert(r.err == EBADF);
}

#define FAKE_N 1300 /* more than two fake light days and many soil cycles */

static void test_fake_light(void)
{
    static double a[FAKE_N], b[FAKE_N];
    read_fake(SENSOR_LIGHT, a, FAKE_N);
    read_fake(SENSOR_LIGHT, b, FAKE_N);

    double lo = a[0], hi = a[0];
    for (int i = 0; i < FAKE_N; i++) {
        assert(a[i] == b[i]); /* deterministic */
        assert(a[i] >= 0.0 && a[i] <= 2000.0);
        if (a[i] < lo) lo = a[i];
        if (a[i] > hi) hi = a[i];
    }
    assert(lo < 10.0);   /* has a night */
    assert(hi > 1000.0); /* has a bright day */
}

static void test_fake_soil(void)
{
    static double a[FAKE_N], b[FAKE_N];
    read_fake(SENSOR_SOIL, a, FAKE_N);
    read_fake(SENSOR_SOIL, b, FAKE_N);

    int waterings = 0;
    for (int i = 0; i < FAKE_N; i++) {
        assert(a[i] == b[i]); /* deterministic */
        assert(a[i] >= 0.0 && a[i] <= 100.0);
        if (i > 0) {
            assert(a[i] != a[i - 1]); /* never stuck */
            if (a[i] - a[i - 1] > 20.0)
                waterings++;
        }
    }
    assert(waterings > 0); /* dries out, then gets watered */
}

static void test_fake_kinds_differ(void)
{
    double light[1], soil[1];
    read_fake(SENSOR_LIGHT, light, 1);
    read_fake(SENSOR_SOIL, soil, 1);
    assert(light[0] != soil[0]);
}

static void test_bad_name(void)
{
    struct backend b;
    assert(backend_init(&b, "bogus", SENSOR_LIGHT) == -EINVAL);
    assert(b.ops == NULL);
    assert(backend_read(&b).status == BACKEND_FAIL);
    assert(backend_close(&b) == 0);

    /* Unknown sensor kind. */
    assert(backend_init(&b, "fake", (enum sensor_kind)0) == -EINVAL);
    assert(b.ops == NULL);
    assert(backend_init(&b, "fake", (enum sensor_kind)3) == -EINVAL);
    assert(b.ops == NULL);

    /* The real backend is not implemented yet: it must fail, never invent values. */
    assert(backend_init(&b, "real", SENSOR_LIGHT) == -ENOSYS);
    assert(b.ops == NULL);
}

/*
 * Run a sensor binary with one or two arguments (arg2 may be NULL); return
 * its exit status and put what it wrote to stderr in err.
 */
static int run_sensor2(const char *sensor, const char *arg, const char *arg2,
                       char *err, size_t errsz)
{
    int pfd[2];
    assert(pipe(pfd) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        if (dup2(pfd[1], STDERR_FILENO) < 0)
            _exit(127);
        close(pfd[0]);
        close(pfd[1]);
        execl(sensor, sensor, arg, arg2, (char *)NULL);
        _exit(127);
    }
    assert(close(pfd[1]) == 0);

    size_t len = 0;
    ssize_t n;
    while ((n = read(pfd[0], err + len, errsz - 1 - len)) > 0)
        len += (size_t)n;
    assert(n == 0);
    err[len] = '\0';
    assert(close(pfd[0]) == 0);

    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    return WEXITSTATUS(status);
}

static int run_sensor(const char *sensor, const char *arg, char *err, size_t errsz)
{
    return run_sensor2(sensor, arg, NULL, err, errsz);
}

static void test_sensor_args(const char *argv0, const char *proc)
{
    /* build/bin/tests/test_backend -> build/bin/<proc> */
    char sensor[512];
    const char *slash = strrchr(argv0, '/');
    assert(slash != NULL);
    int n = snprintf(sensor, sizeof sensor, "%.*s/../%s", (int)(slash - argv0), argv0, proc);
    assert(n > 0 && (size_t)n < sizeof sensor);
    assert(access(sensor, X_OK) == 0);

    char err[4096];

    assert(run_sensor(sensor, "--backend=bogus", err, sizeof err) == 2);
    assert(strstr(err, "invalid --backend value 'bogus'") != NULL);
    assert(strstr(err, "expected real|fake") != NULL);

    assert(run_sensor(sensor, "--backend=", err, sizeof err) == 2);
    assert(strstr(err, "invalid --backend value ''") != NULL);

    assert(run_sensor(sensor, "--bogus-option", err, sizeof err) == 2);
    assert(strstr(err, "usage:") != NULL);

    assert(run_sensor(sensor, "--count=-1", err, sizeof err) == 2);
    assert(strstr(err, "invalid --count value '-1'") != NULL);
    assert(run_sensor(sensor, "--period-ms=0", err, sizeof err) == 2);
    assert(strstr(err, "invalid --period-ms value '0'") != NULL);
    assert(run_sensor(sensor, "--period-ms=abc", err, sizeof err) == 2);
    assert(run_sensor(sensor, "--heartbeat-ms=0", err, sizeof err) == 2);
    assert(strstr(err, "invalid --heartbeat-ms value '0'") != NULL);
    assert(run_sensor(sensor, "--heartbeat-ms=", err, sizeof err) == 2);

    /* Default backend is real, which is not implemented: runtime failure,
     * logged, and no readings sent. */
    assert(run_sensor(sensor, "--socket=/nonexistent/hub.sock", err, sizeof err) == 1);
    assert(strstr(err, "SENSOR_ERROR op=init backend=real") != NULL);

    /* Fake backend with no hub listening: init works, connect fails. A full
     * fake run against a hub is covered by test_pipeline. */
    assert(run_sensor2(sensor, "--backend=fake", "--socket=/nonexistent/hub.sock",
                       err, sizeof err) == 1);
    assert(strstr(err, "op=connect") != NULL);
    assert(strstr(err, "] START") == NULL);

    assert(run_sensor(sensor, "--help", err, sizeof err) == 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    test_find();
    test_fake_light();
    test_fake_soil();
    test_fake_kinds_differ();
    test_bad_name();
    test_sensor_args(argv[0], "light_proc");
    test_sensor_args(argv[0], "soil_proc");
    printf("test_backend: all asserts passed\n");
    return 0;
}
