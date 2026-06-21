#ifndef QTTYFORGE_CONFIG_H
#define QTTYFORGE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

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

/* Default config path when neither --config nor --no-config is given. */
#define CONFIG_DEFAULT_PATH "/etc/config/qttyforge"

/* Initialise cfg with built-in defaults (all strings heap-allocated). */
void config_init(struct config *cfg);

/* Free everything owned by cfg (does not free cfg itself). */
void config_free(struct config *cfg);

/*
 * Overlay a UCI-format file onto cfg. Returns 0 on success, -1 if the
 * file cannot be opened (errno set; caller decides how loud to be).
 * Malformed lines are warned about and skipped, never fatal.
 */
int config_load_file(struct config *cfg, const char *path);

/* Append a new AT mapping (enabled by default); returns the new entry. */
struct at_map *config_add_at(struct config *cfg, const char *name);

/* Log the fully resolved config (placeholder until the relay engine exists). */
void config_dump(const struct config *cfg);

#endif /* QTTYFORGE_CONFIG_H */
