#ifndef QTTYFORGE_CLI_H
#define QTTYFORGE_CLI_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>

/* A --at smd:tty mapping queued on the command line. */
struct cli_at {
	char *smd;
	char *tty;
};

/*
 * Parsed command line. Scalar override strings are borrowed pointers into
 * argv (valid for the program's lifetime); the cli_at array is owned and
 * freed by cli_opts_free().
 */
struct cli_opts {
	const char *config_path;	/* NULL => --no-config (defaults + args only) */
	bool config_explicit;		/* -c/--config was given explicitly */
	bool debug;

	/* DIAG-leg overrides; NULL/false means "leave the config value". */
	bool        no_diag;
	const char *diag_tty;
	const char *diag_socket;
	const char *diag_router;

	struct cli_at *ats;		/* from --at (appended to config ATs) */
	size_t n_ats;
	size_t cap_ats;
};

/*
 * Parse argv into opts. On --help/--version this prints and sets *done so
 * the caller exits 0. Returns 0 on success, -1 on a usage error (message
 * already printed). Intended flow:
 *   cli_parse() -> config_init() -> config_load_file() (unless --no-config)
 *   -> cli_apply() -> run.
 */
int cli_parse(int argc, char **argv, struct cli_opts *opts, bool *done);

/* Overlay the captured CLI overrides on top of cfg. */
void cli_apply(struct config *cfg, const struct cli_opts *opts);

/* Free memory owned by opts (the cli_at array). */
void cli_opts_free(struct cli_opts *opts);

#endif /* QTTYFORGE_CLI_H */
