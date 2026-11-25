#include "backend.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Fake backend: a deterministic triangle wave between FAKE_MIN and
 * FAKE_MAX, one step per read, so runs are repeatable without hardware.
 */

#define FAKE_MIN    20.0
#define FAKE_STEPS  60 /* reads from min to max */

struct fake_state {
    uint64_t reads;
};

static int fake_init(void **state)
{
    struct fake_state *s = calloc(1, sizeof *s);
    if (s == NULL)
        return -ENOMEM;
    *state = s;
    return 0;
}

static struct backend_reading fake_read(void *state)
{
    struct fake_state *s = state;
    uint64_t phase = s->reads++ % (2 * FAKE_STEPS);
    uint64_t step = phase < FAKE_STEPS ? phase : 2 * FAKE_STEPS - phase;
    return (struct backend_reading){
        .status = BACKEND_OK,
        .value  = FAKE_MIN + (double)step,
    };
}

static int fake_close(void *state)
{
    free(state);
    return 0;
}

const struct backend_ops backend_fake_ops = {
    .name      = "fake",
    .simulated = 1,
    .init      = fake_init,
    .read      = fake_read,
    .close     = fake_close,
};
