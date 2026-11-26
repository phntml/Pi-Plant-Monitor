#define _GNU_SOURCE /* getopt_long, ppoll */

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "msg.h"
#include "sock.h"
#include "store.h"

/*
 * Hub process (minimal): accepts sensor connections on a Unix socket and
 * appends every reading to a CSV through the store.
 *
 * Usage: hub [--socket=PATH] [--csv=PATH]
 *
 * Exit codes:
 *   0  clean exit (SIGTERM/SIGINT)
 *   1  runtime failure (socket or CSV could not be opened)
 *   2  usage error
 *
 * Not yet: staleness/sequence-gap checks, analysis, heartbeats, batching.
 */

#define EXIT_OK      0
#define EXIT_RUNTIME 1
#define EXIT_USAGE   2

#define HUB_SOCKET_DEFAULT "hub.sock"
#define HUB_CSV_DEFAULT    "readings.csv"
#define HUB_MAX_CLIENTS    8
#define HUB_BACKLOG        8

struct client {
    int fd;             /* -1: slot free */
    uint32_t sensor_id; /* 0 until the first valid reading */
};

static volatile sig_atomic_t stop_requested;

static void on_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: hub [--socket=PATH] [--csv=PATH]  (defaults: %s, %s)\n",
            HUB_SOCKET_DEFAULT, HUB_CSV_DEFAULT);
}

static const char *client_name(const struct client *c)
{
    const char *n = sensor_kind_name(c->sensor_id);
    return n != NULL ? n : "unknown";
}

static void drop_client(struct client *c)
{
    log_info(EV_DISCONNECT, "sensor=%s fd=%d", client_name(c), c->fd);
    if (sock_close(c->fd) < 0)
        log_warn(EV_SYSCALL_FAIL, "op=close fd=%d", c->fd);
    c->fd = -1;
    c->sensor_id = 0;
}

static void accept_client(int listen_fd, struct client *clients)
{
    int fd = sock_accept(listen_fd);
    if (fd < 0) {
        log_warn(EV_SYSCALL_FAIL, "op=accept err=%s", strerror(-fd));
        return;
    }
    for (int i = 0; i < HUB_MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) {
            clients[i].fd = fd;
            clients[i].sensor_id = 0;
            log_info(EV_CONNECT, "fd=%d", fd);
            return;
        }
    }
    log_warn(EV_CONNECT, "fd=%d rejected=too_many_clients max=%d", fd, HUB_MAX_CLIENTS);
    (void)sock_close(fd);
}

static void handle_client(struct client *c, int store_fd)
{
    struct msg m;
    int rc = sock_recv(c->fd, &m);
    if (rc == SOCK_DISCONNECTED) {
        drop_client(c);
        return;
    }
    if (rc == -EMSGSIZE || rc == -EBADMSG) {
        log_warn(EV_BAD_MSG, "from=%s reason=size", client_name(c));
        return;
    }
    if (rc < 0) {
        log_warn(EV_SYSCALL_FAIL, "op=recv sensor=%s err=%s", client_name(c), strerror(-rc));
        drop_client(c);
        return;
    }

    switch (m.type) {
    case MSG_READING:
        break;
    case MSG_HEARTBEAT:
        return;
    default:
        log_warn(EV_BAD_MSG, "from=%s reason=type type=%u", client_name(c), m.type);
        return;
    }

    const struct msg_reading *r = &m.u.reading;
    if (sensor_kind_name(r->sensor_id) == NULL) {
        log_warn(EV_BAD_MSG, "from=%s reason=sensor_id sensor_id=%u",
                 client_name(c), r->sensor_id);
        return;
    }
    if (c->sensor_id == 0)
        c->sensor_id = r->sensor_id;

    rc = store_append(store_fd, r);
    if (rc < 0)
        log_error(EV_STORE_ERROR, "sensor=%s seq=%u err=%s",
                  client_name(c), r->seq, strerror(-rc));
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "socket", required_argument, NULL, 's' },
        { "csv",    required_argument, NULL, 'c' },
        { "help",   no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    const char *socket_path = HUB_SOCKET_DEFAULT;
    const char *csv_path = HUB_CSV_DEFAULT;

    int opt;
    while ((opt = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'c':
            csv_path = optarg;
            break;
        case 'h':
            usage(stdout);
            return EXIT_OK;
        default:
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "hub: unexpected argument '%s'\n", argv[optind]);
        usage(stderr);
        return EXIT_USAGE;
    }

    if (log_open(NULL, "hub") < 0)
        return EXIT_RUNTIME;

    int exit_code = EXIT_RUNTIME;
    int listen_fd = -1;
    struct client clients[HUB_MAX_CLIENTS];
    for (int i = 0; i < HUB_MAX_CLIENTS; i++)
        clients[i] = (struct client){ .fd = -1, .sensor_id = 0 };

    /* Block the stop signals except inside ppoll, so a signal can't slip in
     * between checking stop_requested and going to sleep. */
    sigset_t block, wait_mask;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    if (sigprocmask(SIG_BLOCK, &block, &wait_mask) < 0) {
        log_error(EV_SYSCALL_FAIL, "op=sigprocmask err=%s", strerror(errno));
        return EXIT_RUNTIME;
    }
    sigdelset(&wait_mask, SIGTERM);
    sigdelset(&wait_mask, SIGINT);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) < 0 || sigaction(SIGINT, &sa, NULL) < 0) {
        log_error(EV_SYSCALL_FAIL, "op=sigaction err=%s", strerror(errno));
        return EXIT_RUNTIME;
    }

    int store_fd = store_open(csv_path);
    if (store_fd < 0) {
        log_error(EV_STORE_ERROR, "op=open csv=%s err=%s", csv_path, strerror(-store_fd));
        return EXIT_RUNTIME;
    }

    listen_fd = sock_listen(socket_path, HUB_BACKLOG);
    if (listen_fd < 0) {
        log_error(EV_SYSCALL_FAIL, "op=listen socket=%s err=%s",
                  socket_path, strerror(-listen_fd));
        goto cleanup;
    }
    log_info(EV_START, "socket=%s csv=%s", socket_path, csv_path);

    while (!stop_requested) {
        struct pollfd pfds[1 + HUB_MAX_CLIENTS];
        pfds[0] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
        for (int i = 0; i < HUB_MAX_CLIENTS; i++)
            pfds[1 + i] = (struct pollfd){ .fd = clients[i].fd, .events = POLLIN };

        int n = ppoll(pfds, 1 + HUB_MAX_CLIENTS, NULL, &wait_mask);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            log_error(EV_SYSCALL_FAIL, "op=ppoll err=%s", strerror(errno));
            goto cleanup;
        }

        for (int i = 0; i < HUB_MAX_CLIENTS; i++)
            if (clients[i].fd >= 0 && pfds[1 + i].revents != 0)
                handle_client(&clients[i], store_fd);
        if (pfds[0].revents & POLLIN)
            accept_client(listen_fd, clients);
    }
    log_info(EV_SIGNAL, "stopping");
    exit_code = EXIT_OK;

cleanup:
    for (int i = 0; i < HUB_MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            drop_client(&clients[i]);
    if (listen_fd >= 0) {
        if (sock_close(listen_fd) < 0)
            exit_code = EXIT_RUNTIME;
        if (unlink(socket_path) < 0) {
            log_warn(EV_SYSCALL_FAIL, "op=unlink socket=%s err=%s",
                     socket_path, strerror(errno));
            exit_code = EXIT_RUNTIME;
        }
    }
    int rc = store_close(store_fd);
    if (rc < 0) {
        log_error(EV_STORE_ERROR, "op=close csv=%s err=%s", csv_path, strerror(-rc));
        exit_code = EXIT_RUNTIME;
    }
    if (exit_code == EXIT_OK)
        log_info(EV_STOP, NULL);
    return exit_code;
}
