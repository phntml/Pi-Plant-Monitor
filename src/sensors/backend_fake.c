#include "backend.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * Fake backend: deterministic synthetic values for running the pipeline and
 * fault tests without hardware. The sensor process flags every reading from
 * it MSG_FLAG_SIMULATED (.simulated below), so it shows as SYNTHETIC in the
 * CSV and can never be mistaken for a real measurement.
 *
 * Every instance of a kind produces the same sequence (fixed-seed jitter),
 * so runs are repeatable.
 *
 *   light  a fake "day" of FAKE_DAY_READS reads: the first half is daylight
 *          (a parabola peaking near FAKE_LUX_PEAK), the second half is night
 *          near FAKE_LUX_NIGHT; +-2% jitter; clamped to [0, FAKE_LUX_MAX].
 *   soil   starts at FAKE_SOIL_WET %, dries FAKE_SOIL_DRY_STEP per read down
 *          to FAKE_SOIL_DRY, then is "watered" back to FAKE_SOIL_WET;
 *          +-0.3 jitter; clamped to [0, 100]. Consecutive values always
 *          differ, so stuck-value detection never fires on fake data.
 */

#define FAKE_DAY_READS     600
#define FAKE_LUX_PEAK      1500.0
#define FAKE_LUX_NIGHT     2.5
#define FAKE_LUX_MAX       2000.0
#define FAKE_LUX_JITTER    0.02 /* fraction of the value */

#define FAKE_SOIL_WET       70.0
#define FAKE_SOIL_DRY       25.0
#define FAKE_SOIL_DRY_STEP  0.5
#define FAKE_SOIL_JITTER    0.3 /* percentage points */
#define FAKE_SOIL_NUDGE     0.01
#define FAKE_SOIL_CYCLE     ((uint64_t)((FAKE_SOIL_WET - FAKE_SOIL_DRY) / FAKE_SOIL_DRY_STEP) + 1)

#define FAKE_SEED UINT64_C(0x9E3779B97F4A7C15)

struct fake_state {
    enum sensor_kind kind;
    uint64_t reads;
    uint64_t rng;
    double   prev;
    int      have_prev;
};

/* xorshift64; returns a uniform value in [-1, 1]. */
static double jitter(struct fake_state *s)
{
    uint64_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s->rng = x;
    return (double)(x >> 11) / (double)(UINT64_C(1) << 52) - 1.0;
}

static double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static double fake_light(struct fake_state *s)
{
    uint64_t phase = s->reads % FAKE_DAY_READS;
    double base = FAKE_LUX_NIGHT;
    if (phase < FAKE_DAY_READS / 2) {
        double t = (double)phase / (FAKE_DAY_READS / 2);
        base += FAKE_LUX_PEAK * 4.0 * t * (1.0 - t);
    }
    double v = base * (1.0 + FAKE_LUX_JITTER * jitter(s));
    return clamp(v, 0.0, FAKE_LUX_MAX);
}

static double fake_soil(struct fake_state *s)
{
    uint64_t step = s->reads % FAKE_SOIL_CYCLE;
    double base = FAKE_SOIL_WET - FAKE_SOIL_DRY_STEP * (double)step;
    double v = clamp(base + FAKE_SOIL_JITTER * jitter(s), 0.0, 100.0);
    if (s->have_prev && v == s->prev)
        v = v + FAKE_SOIL_NUDGE <= 100.0 ? v + FAKE_SOIL_NUDGE : v - FAKE_SOIL_NUDGE;
    return v;
}

static int fake_init(void **state, enum sensor_kind kind)
{
    struct fake_state *s = calloc(1, sizeof *s);
    if (s == NULL)
        return -ENOMEM;
    s->kind = kind;
    s->rng = FAKE_SEED;
    *state = s;
    return 0;
}

static struct backend_reading fake_read(void *state)
{
    struct fake_state *s = state;
    double v;
    switch (s->kind) {
    case SENSOR_LIGHT:
        v = fake_light(s);
        break;
    case SENSOR_SOIL:
        v = fake_soil(s);
        break;
    default:
        return (struct backend_reading){ .status = BACKEND_FAIL, .err = EINVAL };
    }
    s->reads++;
    s->prev = v;
    s->have_prev = 1;
    return (struct backend_reading){ .status = BACKEND_OK, .value = v };
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
