#include "backend.h"

#include <errno.h>
#include <stddef.h>

/*
 * Real backend: the physical sensor.
 *
 * Not implemented yet: the hardware interface (bus, device, wiring) has not
 * been chosen. init fails with -ENOSYS so a sensor started with the default
 * backend exits with a clear error instead of reporting made-up values.
 */

static int real_init(void **state)
{
    *state = NULL;
    return -ENOSYS;
}

static struct backend_reading real_read(void *state)
{
    (void)state;
    return (struct backend_reading){ .status = BACKEND_FAIL, .err = ENOSYS };
}

static int real_close(void *state)
{
    (void)state;
    return 0;
}

const struct backend_ops backend_real_ops = {
    .name      = "real",
    .simulated = 0,
    .init      = real_init,
    .read      = real_read,
    .close     = real_close,
};
