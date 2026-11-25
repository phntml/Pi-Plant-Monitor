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

static void test_fake(void)
{
    struct backend b;
    assert(backend_init(&b, "fake") == 0);
    assert(b.ops == backend_find("fake"));

    double prev = 0;
    for (int i = 0; i < 300; i++) {
        struct backend_reading r = backend_read(&b);
        assert(r.status == BACKEND_OK);
        assert(r.err == 0);
        assert(r.value >= 20.0 && r.value <= 80.0);
        if (i > 0) /* triangle wave: moves one step per read */
            assert(r.value - prev == 1.0 || prev - r.value == 1.0);
        prev = r.value;
    }

    assert(backend_close(&b) == 0);
    assert(b.ops == NULL);
    assert(backend_close(&b) == 0); /* idempotent */

    /* A closed backend reports a failed reading, not a value. */
    struct backend_reading r = backend_read(&b);
    assert(r.status == BACKEND_FAIL);
    assert(r.err == EBADF);
}

static void test_bad_name(void)
{
    struct backend b;
    assert(backend_init(&b, "bogus") == -EINVAL);
    assert(b.ops == NULL);
    assert(backend_read(&b).status == BACKEND_FAIL);
    assert(backend_close(&b) == 0);
}

/*
 * Run the sensor binary with one argument; return its exit status and put
 * what it wrote to stderr in err.
 */
static int run_sensor(const char *sensor, const char *arg, char *err, size_t errsz)
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
        execl(sensor, "sensor", arg, (char *)NULL);
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

static void test_sensor_args(const char *argv0)
{
    /* build/bin/tests/test_backend -> build/bin/sensor */
    char sensor[512];
    const char *slash = strrchr(argv0, '/');
    assert(slash != NULL);
    int n = snprintf(sensor, sizeof sensor, "%.*s/../sensor", (int)(slash - argv0), argv0);
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

    assert(run_sensor(sensor, "--backend=fake", err, sizeof err) == 0);
    assert(strstr(err, "] START backend=fake\n") != NULL);
    assert(strstr(err, "] STOP\n") != NULL);

    assert(run_sensor(sensor, "--help", err, sizeof err) == 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    test_find();
    test_fake();
    test_bad_name();
    test_sensor_args(argv[0]);
    printf("test_backend: all asserts passed\n");
    return 0;
}
