#define _GNU_SOURCE /* SOCK_CLOEXEC */
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "msg.h"
#include "sock.h"

/* Send m from one end of the pair and check the other end gets the same bytes. */
static void roundtrip(int from, int to, const struct msg *m)
{
    struct msg got;
    memset(&got, 0xA5, sizeof got); /* so a field that isn't copied shows up */
    assert(sock_send(from, m) == 0);
    assert(sock_recv(to, &got) == 0);
    assert(memcmp(&got, m, sizeof got) == 0);
}

int main(void)
{
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);

    struct msg reading;
    memset(&reading, 0, sizeof reading);
    reading.type = MSG_READING;
    reading.u.reading.sensor_id = 1;
    reading.u.reading.seq = 42;
    reading.u.reading.timestamp_ns = INT64_C(1764000000123456789);
    reading.u.reading.value = 37.25;
    reading.u.reading.flags = MSG_FLAG_SIMULATED;

    struct msg hb;
    memset(&hb, 0, sizeof hb);
    hb.type = MSG_HEARTBEAT;
    hb.u.heartbeat.pid = (int32_t)getpid();
    hb.u.heartbeat.reading_count = 43;

    /* Sensor end -> hub end. */
    roundtrip(sv[0], sv[1], &reading);
    roundtrip(sv[0], sv[1], &hb);

    /* Field-by-field too, so a failure names what differs. */
    struct msg got;
    assert(sock_send(sv[0], &reading) == 0);
    assert(sock_send(sv[0], &hb) == 0);
    assert(sock_recv(sv[1], &got) == 0);
    assert(got.type == MSG_READING);
    assert(got.u.reading.sensor_id == 1);
    assert(got.u.reading.seq == 42);
    assert(got.u.reading.timestamp_ns == INT64_C(1764000000123456789));
    assert(got.u.reading.value == 37.25);
    assert(got.u.reading.flags == MSG_FLAG_SIMULATED);
    assert(sock_recv(sv[1], &got) == 0);
    assert(got.type == MSG_HEARTBEAT);
    assert(got.u.heartbeat.pid == (int32_t)getpid());
    assert(got.u.heartbeat.reading_count == 43);

    /* The pair works in both directions. */
    roundtrip(sv[1], sv[0], &hb);

    assert(sock_close(sv[0]) == 0);
    assert(sock_recv(sv[1], &got) == SOCK_DISCONNECTED);
    assert(sock_close(sv[1]) == 0);

    printf("test_sockpair: all asserts passed\n");
    return 0;
}
