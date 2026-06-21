#ifndef QTTYFORGE_H
#define QTTYFORGE_H

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ version */

#define QTF_NAME    "qttyforge"
#define QTF_VERSION "0.1.0-dev"

/* --------------------------------------------------------------------- log */

enum log_level {
	LOG_ERR,
	LOG_WARN,
	LOG_INFO,
	LOG_DBG,
};

void log_init(bool debug);
void log_msg(enum log_level level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define log_err(...)  log_msg(LOG_ERR,  __VA_ARGS__)
#define log_warn(...) log_msg(LOG_WARN, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_dbg(...)  log_msg(LOG_DBG,  __VA_ARGS__)

/* ------------------------------------------------------------------- xalloc */
/* Allocation wrappers that log and abort on failure (config/arg data is tiny
 * and parsed once, so OOM there is fatal — keeps call sites clean). */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ------------------------------------------------------------------- config */

/* One internal AT channel -> local PTY mapping (repeatable). */
struct at_map {
	char *name;	/* section name, e.g. "at0" (for logging) */
	bool  enabled;
	char *smd;	/* internal channel device, e.g. /dev/smd9 */
	char *tty;	/* local PTY symlink, e.g. /dev/ttyAT0 */
};

/* The single DIAG/DM leg. */
struct diag_cfg {
	bool  enabled;	/* ttyDiag on/off */
	char *tty;	/* e.g. /dev/ttyDiag */
	char *socket;	/* addr:port for the `diag-router -s` interject */
	char *router;	/* diag-router binary path; NULL = auto-detect */
};

struct config {
	bool enabled;		/* global master switch: daemon runs or not */
	struct diag_cfg diag;
	struct at_map *ats;	/* dynamic array of AT mappings */
	size_t n_ats;
	size_t cap_ats;
};

#define CONFIG_DEFAULT_PATH "/etc/config/qttyforge"

void config_init(struct config *cfg);
void config_free(struct config *cfg);
int  config_load_file(struct config *cfg, const char *path);
struct at_map *config_add_at(struct config *cfg, const char *name);
void config_dump(const struct config *cfg);

/* ---------------------------------------------------------------------- cli */

/* A --at smd:tty mapping queued on the command line. */
struct cli_at {
	char *smd;
	char *tty;
};

/* Parsed command line. Scalar override strings are borrowed pointers into
 * argv; the cli_at array is owned and freed by cli_opts_free(). */
struct cli_opts {
	const char *config_path;	/* NULL => --no-config (defaults + args only) */
	bool config_explicit;		/* -c/--config given explicitly */
	bool debug;

	bool        no_diag;		/* DIAG-leg overrides; NULL/false = leave config */
	const char *diag_tty;
	const char *diag_socket;
	const char *diag_router;

	struct cli_at *ats;		/* from --at (appended to config ATs) */
	size_t n_ats;
	size_t cap_ats;
};

int  cli_parse(int argc, char **argv, struct cli_opts *opts, bool *done);
void cli_apply(struct config *cfg, const struct cli_opts *opts);
void cli_opts_free(struct cli_opts *opts);

/* --------------------------------------------------------------- relay/pty */

/* The relay engine: a single poll() loop that pumps bytes bidirectionally
 * between fd pairs, with per-direction buffering and backpressure. Dumb by
 * design — it never inspects or rewrites the stream. Both the AT leg
 * (smd <-> pty) and the DIAG leg (tcp <-> pty) are built on it. */
struct engine;

struct engine *engine_new(void);
void engine_free(struct engine *e);

/* Register a bidirectional relay between two open fds. The engine takes
 * ownership of fd_a, fd_b, and hold_fd (an extra fd kept open for the
 * relay's lifetime, e.g. a pty slave; -1 if none), closing them on
 * teardown. If unlink_path is non-NULL it is unlink()ed on teardown (its
 * own copy is kept). Both fds are set non-blocking. Returns 0 / -1. */
int  engine_add_relay(struct engine *e, const char *name, int fd_a, int fd_b,
		      int hold_fd, const char *unlink_path);

/* Run the poll() loop until SIGINT/SIGTERM or all relays close. Returns 0. */
int  engine_run(struct engine *e);

/* Number of relays registered. */
size_t engine_count(const struct engine *e);

struct pty {
	int master;
	int slave;
	char *link;	/* symlink path created (heap), or NULL */
};

/* Create a PTY, raw slave, symlink link_path -> slave, hold-open friendly.
 * master is set non-blocking. Returns 0 / -1. */
int  pty_open(struct pty *p, const char *link_path);
void pty_close(struct pty *p);

/* ------------------------------------------------------------------ AT leg */

/* Bring up every enabled AT channel in cfg (open smd, create pty, register
 * relay). Per-channel failures are non-fatal. Returns the count started. */
int at_start_all(struct engine *e, const struct config *cfg);

/* ---------------------------------------------------------------- DIAG leg */

/* Interject the managed diag-router to run with -s, accept its socket, and
 * relay it to /dev/ttyDiag via the engine. Returns 0 if DIAG came up, -1 if
 * skipped (non-fatal). diag_stop() reverts the interject on shutdown and is
 * safe to call even if diag_start() was never called or failed. */
int  diag_start(struct engine *e, const struct config *cfg);
void diag_stop(void);

#endif /* QTTYFORGE_H */
