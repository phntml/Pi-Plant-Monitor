#include "backend.h"

#include <errno.h>
#include <string.h>

extern const struct backend_ops backend_real_ops;
extern const struct backend_ops backend_fake_ops;

static const struct backend_ops *const backends[] = {
    &backend_real_ops,
    &backend_fake_ops,
};

const struct backend_ops *backend_find(const char *name)
{
    if (name == NULL)
        name = BACKEND_DEFAULT;
    for (size_t i = 0; i < sizeof backends / sizeof backends[0]; i++)
        if (strcmp(backends[i]->name, name) == 0)
            return backends[i];
    return NULL;
}

const char *backend_names(void)
{
    return "real|fake";
}

int backend_init(struct backend *b, const char *name)
{
    b->ops = NULL;
    b->state = NULL;

    const struct backend_ops *ops = backend_find(name);
    if (ops == NULL)
        return -EINVAL;

    void *state = NULL;
    int rc = ops->init(&state);
    if (rc < 0)
        return rc;

    b->ops = ops;
    b->state = state;
    return 0;
}

struct backend_reading backend_read(struct backend *b)
{
    if (b->ops == NULL)
        return (struct backend_reading){ .status = BACKEND_FAIL, .err = EBADF };
    return b->ops->read(b->state);
}

int backend_close(struct backend *b)
{
    if (b->ops == NULL)
        return 0;
    int rc = b->ops->close(b->state);
    b->ops = NULL;
    b->state = NULL;
    return rc;
}
