#define _XOPEN_SOURCE 700 /* realpath */
#undef NDEBUG
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Acceptance test for the fake backend: hub + light_proc + soil_proc, all
 * with --backend=fake, and every row in the resulting CSV flagged SYNTHETIC.
 *
 * Children run with the temp dir as cwd so the socket path stays short
 * (sun_path is limited to 108 bytes).
 */

#define COUNT 5
#define PERIOD_NS 10000000LL /* --period-ms=10 */
#define JITTER_NS 50000000LL

static char bindir[512];
static char tmpdir[] = "/tmp/test_pipeline-XXXXXX";

/* Start bin/<prog> in tmpdir with up to 4 args. If err_fd is not NULL, the
 * child's stderr goes to a pipe whose read end is stored there. */
static pid_t spawn(const char *prog, const char *const args[4], int *err_fd)
{
    char path[600];
    int n = snprintf(path, sizeof path, "%s/%s", bindir, prog);
    assert(n > 0 && (size_t)n < sizeof path);
    assert(access(path, X_OK) == 0);

    int pfd[2] = { -1, -1 };
    if (err_fd != NULL)
        assert(pipe(pfd) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        if (err_fd != NULL) {
            if (dup2(pfd[1], STDERR_FILENO) < 0)
                _exit(127);
            close(pfd[0]);
            close(pfd[1]);
        }
        if (chdir(tmpdir) < 0)
            _exit(127);
        execl(path, prog, args[0], args[1], args[2], args[3], (char *)NULL);
        _exit(127);
    }
    if (err_fd != NULL) {
        assert(close(pfd[1]) == 0);
        *err_fd = pfd[0];
    }
    return pid;
}

static int wait_exit(pid_t pid)
{
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    return WEXITSTATUS(status);
}

/* Read the hub's stderr until `needle` has appeared `times` times. */
static void wait_for(FILE *hub_err, const char *needle, int times)
{
    char line[1024];
    int seen = 0;
    while (seen < times) {
        assert(fgets(line, sizeof line, hub_err) != NULL); /* hub must not exit */
        fputs(line, stderr);
        if (strstr(line, needle) != NULL)
            seen++;
    }
}

static void check_csv(void)
{
    char path[64];
    assert(snprintf(path, sizeof path, "%s/r.csv", tmpdir) < (int)sizeof path);
    FILE *f = fopen(path, "r");
    assert(f != NULL);

    char line[256];
    assert(fgets(line, sizeof line, f) != NULL);
    assert(strcmp(line, "wall_time,mono_ns,sensor,seq,value,flags\n") == 0);

    int rows = 0, next_seq[2] = { 0, 0 };
    long long prev_mono[2] = { 0, 0 };
    /* Schedule error e = mono_ns - seq * period, relative to an unknown start.
     * Readings sit on a fixed grid, so e stays within a narrow band; a
     * drifting loop would make it grow with seq. */
    long long err_min[2] = { 0, 0 }, err_max[2] = { 0, 0 };
    while (fgets(line, sizeof line, f) != NULL) {
        rows++;
        assert(strchr(line, '\n') != NULL);

        char ts[32], sensor[16], flags[64];
        long long mono;
        unsigned seq;
        double value;
        int n = sscanf(line, "%31[^,],%lld,%15[^,],%u,%lf,%63[^\n]",
                       ts, &mono, sensor, &seq, &value, flags);
        assert(n == 6);
        assert(strlen(ts) == 24 && ts[10] == 'T' && ts[23] == 'Z');
        assert(mono > 0);

        /* The acceptance criterion: every fake reading is flagged SYNTHETIC. */
        assert(strcmp(flags, "SYNTHETIC") == 0);

        int k;
        if (strcmp(sensor, "light") == 0) {
            k = 0;
            assert(value >= 0.0 && value <= 2000.0);
        } else {
            assert(strcmp(sensor, "soil") == 0);
            k = 1;
            assert(value >= 0.0 && value <= 100.0);
        }
        assert(seq == (unsigned)next_seq[k]); /* in order, no gaps */
        next_seq[k]++;
        /* mono_ns strictly increases per sensor. */
        if (prev_mono[k] != 0)
            assert(mono > prev_mono[k]);
        prev_mono[k] = mono;
        long long e = mono - (long long)seq * PERIOD_NS;
        if (seq == 0 || e < err_min[k])
            err_min[k] = e;
        if (seq == 0 || e > err_max[k])
            err_max[k] = e;
    }
    assert(rows == 2 * COUNT);
    assert(next_seq[0] == COUNT && next_seq[1] == COUNT);
    for (int k = 0; k < 2; k++) /* within the design's 50 ms jitter budget */
        assert(err_max[k] - err_min[k] < JITTER_NS);
    assert(fclose(f) == 0);
    assert(unlink(path) == 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    /* build/bin/tests/test_pipeline -> build/bin */
    const char *slash = strrchr(argv[0], '/');
    assert(slash != NULL);
    int n = snprintf(bindir, sizeof bindir, "%.*s/..", (int)(slash - argv[0]), argv[0]);
    assert(n > 0 && (size_t)n < sizeof bindir);
    char *abs = realpath(bindir, NULL);
    assert(abs != NULL);
    n = snprintf(bindir, sizeof bindir, "%s", abs);
    assert(n > 0 && (size_t)n < sizeof bindir);
    free(abs);

    assert(mkdtemp(tmpdir) != NULL);

    int hub_err_fd;
    const char *hub_args[4] = { "--socket=h.sock", "--csv=r.csv", NULL, NULL };
    pid_t hub = spawn("hub", hub_args, &hub_err_fd);
    FILE *hub_err = fdopen(hub_err_fd, "r");
    assert(hub_err != NULL);
    wait_for(hub_err, "] START ", 1);

    const char *sensor_args[4] = { "--backend=fake", "--socket=h.sock",
                                   "--count=5", "--period-ms=10" };
    pid_t light = spawn("light_proc", sensor_args, NULL);
    pid_t soil = spawn("soil_proc", sensor_args, NULL);
    assert(wait_exit(light) == 0);
    assert(wait_exit(soil) == 0);

    /* SEQPACKET delivers in order, so once the hub has seen both sensors
     * disconnect it has stored every reading they sent. */
    wait_for(hub_err, "] DISCONNECT ", 2);
    assert(kill(hub, SIGTERM) == 0);
    wait_for(hub_err, "] STOP", 1);
    assert(fclose(hub_err) == 0);
    assert(wait_exit(hub) == 0);

    check_csv();
    char sock[64];
    assert(snprintf(sock, sizeof sock, "%s/h.sock", tmpdir) < (int)sizeof sock);
    assert(access(sock, F_OK) != 0); /* hub removed its socket */
    assert(rmdir(tmpdir) == 0);

    printf("test_pipeline: all asserts passed\n");
    return 0;
}
