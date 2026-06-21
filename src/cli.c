#include "cli.h"
#include "config.h"
#include "log.h"
#include "util.h"
#include "version.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
	fprintf(out,
"Usage: qttyforge [options]\n"
"\n"
"Forge a Qualcomm modem AP's internal DIAG and AT interfaces into local TTYs.\n"
"\n"
"Options:\n"
"  -c, --config PATH       config file (default: %s)\n"
"      --no-config         ignore the config file; use defaults + args only\n"
"      --diag-tty PATH     DIAG PTY path (default from config)\n"
"      --no-diag           disable the DIAG leg\n"
"      --diag-socket A:P   addr:port for the diag-router -s interject\n"
"      --diag-router PATH  diag-router binary (default: auto-detect)\n"
"  -a, --at SMD:TTY        add an AT channel mapping, e.g. /dev/smd9:/dev/ttyAT0\n"
"                          (repeatable; appended to any config sections)\n"
"  -d, --debug             verbose debug logging\n"
"  -h, --help              show this help and exit\n"
"  -V, --version           show version and exit\n",
		CONFIG_DEFAULT_PATH);
}

static int add_at_override(struct cli_opts *o, const char *spec)
{
	const char *colon = strchr(spec, ':');

	if (!colon || colon == spec || colon[1] == '\0') {
		log_err("--at expects SMD:TTY, got '%s'", spec);
		return -1;
	}

	if (o->n_ats == o->cap_ats) {
		size_t cap = o->cap_ats ? o->cap_ats * 2 : 4;

		o->ats = xrealloc(o->ats, cap * sizeof(*o->ats));
		o->cap_ats = cap;
	}

	o->ats[o->n_ats].smd = xstrndup(spec, (size_t)(colon - spec));
	o->ats[o->n_ats].tty = xstrdup(colon + 1);
	o->n_ats++;
	return 0;
}

int cli_parse(int argc, char **argv, struct cli_opts *opts, bool *done)
{
	static const struct option longopts[] = {
		{ "config",      required_argument, 0, 'c' },
		{ "no-config",   no_argument,       0, 'C' },
		{ "diag-tty",    required_argument, 0, 't' },
		{ "no-diag",     no_argument,       0, 'D' },
		{ "diag-socket", required_argument, 0, 's' },
		{ "diag-router", required_argument, 0, 'r' },
		{ "at",          required_argument, 0, 'a' },
		{ "debug",       no_argument,       0, 'd' },
		{ "help",        no_argument,       0, 'h' },
		{ "version",     no_argument,       0, 'V' },
		{ 0, 0, 0, 0 },
	};

	memset(opts, 0, sizeof(*opts));
	opts->config_path = CONFIG_DEFAULT_PATH;
	*done = false;

	int c;

	optind = 1;
	while ((c = getopt_long(argc, argv, "c:a:dhV", longopts, NULL)) != -1) {
		switch (c) {
		case 'c':
			opts->config_path = optarg;
			opts->config_explicit = true;
			break;
		case 'C':
			opts->config_path = NULL;
			break;
		case 't':
			opts->diag_tty = optarg;
			break;
		case 'D':
			opts->no_diag = true;
			break;
		case 's':
			opts->diag_socket = optarg;
			break;
		case 'r':
			opts->diag_router = optarg;
			break;
		case 'a':
			if (add_at_override(opts, optarg) != 0)
				return -1;
			break;
		case 'd':
			opts->debug = true;
			break;
		case 'h':
			usage(stdout);
			*done = true;
			return 0;
		case 'V':
			printf("%s %s\n", QTF_NAME, QTF_VERSION);
			*done = true;
			return 0;
		default:	/* '?' — getopt already printed the error */
			usage(stderr);
			return -1;
		}
	}

	if (optind < argc) {
		log_err("unexpected argument '%s'", argv[optind]);
		usage(stderr);
		return -1;
	}

	return 0;
}

void cli_apply(struct config *cfg, const struct cli_opts *opts)
{
	if (opts->no_diag)
		cfg->diag.enabled = false;
	if (opts->diag_tty) {
		free(cfg->diag.tty);
		cfg->diag.tty = xstrdup(opts->diag_tty);
	}
	if (opts->diag_socket) {
		free(cfg->diag.socket);
		cfg->diag.socket = xstrdup(opts->diag_socket);
	}
	if (opts->diag_router) {
		free(cfg->diag.router);
		cfg->diag.router = xstrdup(opts->diag_router);
	}

	for (size_t i = 0; i < opts->n_ats; i++) {
		struct at_map *at = config_add_at(cfg, NULL);

		at->smd = xstrdup(opts->ats[i].smd);
		at->tty = xstrdup(opts->ats[i].tty);
	}
}

void cli_opts_free(struct cli_opts *opts)
{
	for (size_t i = 0; i < opts->n_ats; i++) {
		free(opts->ats[i].smd);
		free(opts->ats[i].tty);
	}
	free(opts->ats);
	opts->ats = NULL;
	opts->n_ats = 0;
	opts->cap_ats = 0;
}
