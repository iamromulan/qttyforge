#ifndef QTTYFORGE_RELAY_H
#define QTTYFORGE_RELAY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The relay engine: a single poll() loop that pumps bytes bidirectionally
 * between pairs of file descriptors. Both the AT leg (smd <-> pty) and the
 * DIAG leg (tcp <-> pty) are built on it. The pump is dumb — it never
 * inspects or rewrites the byte stream — with per-direction buffering and
 * backpressure (a stalled writer stops its reader, never spins or drops).
 */
struct engine;

struct engine *engine_new(void);
void engine_free(struct engine *e);

/*
 * Register a bidirectional relay between two open fds. The engine takes
 * ownership of fd_a and fd_b (and hold_fd, if >= 0 — an extra fd kept open
 * for the relay's lifetime, e.g. a pty slave) and closes them on teardown.
 * If unlink_path is non-NULL it is unlink()ed on teardown (e.g. a pty
 * symlink); the engine keeps its own copy of the string. Both fds are set
 * non-blocking. Returns 0 on success, -1 on failure.
 */
int engine_add_relay(struct engine *e, const char *name, int fd_a, int fd_b,
		     int hold_fd, const char *unlink_path);

/* Run the poll() loop until SIGINT/SIGTERM or all relays close. Returns 0. */
int engine_run(struct engine *e);

/* Number of relays registered. */
size_t engine_count(const struct engine *e);

#endif /* QTTYFORGE_RELAY_H */
