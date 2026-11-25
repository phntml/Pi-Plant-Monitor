#ifndef BACKEND_H
#define BACKEND_H

/*
 * Sensor backend interface.
 *
 * Every sensor process drives its hardware through this interface and
 * never needs to know which backend it has:
 *
 *   struct backend b;
 *   if (backend_init(&b, name) < 0) ...    name from --backend=real|fake
 *   struct backend_reading r = backend_read(&b);
 *   if (r.status == BACKEND_OK) use r.value; else r.err says why
 *   backend_close(&b);
 *
 * Backends:
 *   real  the physical sensor (default)
 *   fake  deterministic simulated values, no hardware needed
 */

#define BACKEND_DEFAULT "real"

enum backend_status {
    BACKEND_OK   = 0, /* value is a valid reading */
    BACKEND_FAIL = 1, /* read failed; value is not meaningful */
};

struct backend_reading {
    enum backend_status status;
    double value; /* meaningful only when status == BACKEND_OK */
    int    err;   /* positive errno when status == BACKEND_FAIL, else 0 */
};

struct backend_ops {
    const char *name;
    int simulated; /* nonzero: values do not come from hardware */
    /* Set *state for later calls. Returns 0 or -errno. */
    int (*init)(void **state);
    struct backend_reading (*read)(void *state);
    /* Release state. Returns 0 or -errno; state is released either way. */
    int (*close)(void *state);
};

struct backend {
    const struct backend_ops *ops; /* NULL when not initialized */
    void *state;
};

/* Look up a backend by name; NULL name means BACKEND_DEFAULT.
 * Returns NULL for an unknown name. */
const struct backend_ops *backend_find(const char *name);

/* Valid names for messages, e.g. "real|fake". */
const char *backend_names(void);

/*
 * Select and initialize a backend. Returns 0, -EINVAL for an unknown name,
 * or the backend's own -errno from init. On failure b is left uninitialized
 * (backend_read() fails and backend_close() does nothing).
 */
int backend_init(struct backend *b, const char *name);

/* Read once. Fails with err == EBADF if b is not initialized. */
struct backend_reading backend_read(struct backend *b);

/* Close b; safe to call on an uninitialized or closed backend. 0 or -errno. */
int backend_close(struct backend *b);

#endif /* BACKEND_H */
