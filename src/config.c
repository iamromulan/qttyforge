/* Config model + UCI-format parser, and the command-line layer that
 * overlays onto it. */
#include "qttyforge.h"

#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ============================ config model ============================ */

static void set_str(char **dst, const char *val)
{
	free(*dst);
	*dst = val ? xstrdup(val) : NULL;
}

static bool parse_bool(const char *s)
{
	/* UCI-style truthy values; anything else is false. */
	return s && (!strcmp(s, "1") || !strcasecmp(s, "on") ||
		     !strcasecmp(s, "true") || !strcasecmp(s, "yes") ||
		     !strcasecmp(s, "enabled"));
}

void config_init(struct config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->enabled = true;
	cfg->diag.enabled = true;
	cfg->diag.tty = xstrdup("/dev/ttyDiag");
	cfg->diag.socket = xstrdup("127.0.0.1:2500");
	cfg->diag.router = NULL;	/* auto-detect */
}

void config_free(struct config *cfg)
{
	free(cfg->diag.tty);
	free(cfg->diag.socket);
	free(cfg->diag.router);
	for (size_t i = 0; i < cfg->n_ats; i++) {
		free(cfg->ats[i].name);
		free(cfg->ats[i].smd);
		free(cfg->ats[i].tty);
	}
	free(cfg->ats);
	memset(cfg, 0, sizeof(*cfg));
}

struct at_map *config_add_at(struct config *cfg, const char *name)
{
	struct at_map *at;

	if (cfg->n_ats == cfg->cap_ats) {
		size_t cap = cfg->cap_ats ? cfg->cap_ats * 2 : 4;

		cfg->ats = xrealloc(cfg->ats, cap * sizeof(*cfg->ats));
		cfg->cap_ats = cap;
	}

	at = &cfg->ats[cfg->n_ats++];
	memset(at, 0, sizeof(*at));
	at->name = name ? xstrdup(name) : NULL;
	at->enabled = true;	/* on unless the config says otherwise */
	return at;
}

/* ============================= UCI parser ============================= */

/*
 * Read one whitespace/quote-delimited token starting at *p, advancing *p
 * past it. Returns a newly allocated token, or NULL at end-of-line or a
 * '#' comment. Single and double quotes are honoured (no escape sequences,
 * matching how these config files are written in practice).
 */
static char *next_token(const char **p)
{
	const char *s = *p;

	while (*s && isspace((unsigned char)*s))
		s++;

	if (*s == '\0' || *s == '#') {
		*p = s;
		return NULL;
	}

	if (*s == '\'' || *s == '"') {
		char quote = *s++;
		const char *start = s;

		while (*s && *s != quote)
			s++;

		char *tok = xstrndup(start, (size_t)(s - start));

		if (*s == quote)
			s++;
		*p = s;
		return tok;
	}

	const char *start = s;

	while (*s && !isspace((unsigned char)*s) && *s != '#')
		s++;

	char *tok = xstrndup(start, (size_t)(s - start));

	*p = s;
	return tok;
}

enum section {
	SECT_NONE,
	SECT_GLOBAL,	/* config qttyforge */
	SECT_DIAG,	/* config diag */
	SECT_AT,	/* config at */
};

static void apply_option(struct config *cfg, enum section sect,
			 struct at_map *at, const char *key, const char *val,
			 const char *path, int lineno)
{
	switch (sect) {
	case SECT_GLOBAL:
		if (!strcmp(key, "enabled"))
			cfg->enabled = parse_bool(val);
		else
			log_warn("%s:%d: unknown global option '%s'", path, lineno, key);
		break;
	case SECT_DIAG:
		if (!strcmp(key, "enabled"))
			cfg->diag.enabled = parse_bool(val);
		else if (!strcmp(key, "tty"))
			set_str(&cfg->diag.tty, val);
		else if (!strcmp(key, "socket"))
			set_str(&cfg->diag.socket, val);
		else if (!strcmp(key, "router"))
			set_str(&cfg->diag.router, val);
		else
			log_warn("%s:%d: unknown diag option '%s'", path, lineno, key);
		break;
	case SECT_AT:
		if (!at) {
			log_warn("%s:%d: option in malformed 'at' section", path, lineno);
		} else if (!strcmp(key, "enabled")) {
			at->enabled = parse_bool(val);
		} else if (!strcmp(key, "smd")) {
			set_str(&at->smd, val);
		} else if (!strcmp(key, "tty")) {
			set_str(&at->tty, val);
		} else {
			log_warn("%s:%d: unknown at option '%s'", path, lineno, key);
		}
		break;
	case SECT_NONE:
	default:
		log_warn("%s:%d: option '%s' before any section", path, lineno, key);
		break;
	}
}

int config_load_file(struct config *cfg, const char *path)
{
	FILE *f = fopen(path, "r");
	char *line = NULL;
	size_t linecap = 0;
	ssize_t len;
	int lineno = 0;
	enum section sect = SECT_NONE;
	struct at_map *cur_at = NULL;

	if (!f)
		return -1;	/* errno set; caller logs */

	while ((len = getline(&line, &linecap, f)) != -1) {
		const char *p = line;
		char *kw;

		lineno++;
		kw = next_token(&p);
		if (!kw)
			continue;	/* blank or comment-only line */

		if (!strcmp(kw, "config")) {
			char *type = next_token(&p);
			char *name = next_token(&p);

			if (!type) {
				log_warn("%s:%d: 'config' without a type", path, lineno);
				sect = SECT_NONE;
			} else if (!strcmp(type, "qttyforge")) {
				sect = SECT_GLOBAL;
			} else if (!strcmp(type, "diag")) {
				sect = SECT_DIAG;
			} else if (!strcmp(type, "at")) {
				sect = SECT_AT;
				cur_at = config_add_at(cfg, name);
			} else {
				log_warn("%s:%d: unknown section type '%s'", path, lineno, type);
				sect = SECT_NONE;
			}
			free(type);
			free(name);
		} else if (!strcmp(kw, "option")) {
			char *key = next_token(&p);
			char *val = next_token(&p);

			if (key && val)
				apply_option(cfg, sect, cur_at, key, val, path, lineno);
			else
				log_warn("%s:%d: malformed option", path, lineno);
			free(key);
			free(val);
		} else if (!strcmp(kw, "list")) {
			/* qttyforge defines no list options; accept and ignore. */
			char *key = next_token(&p);
			char *val = next_token(&p);

			free(key);
			free(val);
		} else {
			log_warn("%s:%d: unexpected keyword '%s'", path, lineno, kw);
		}
		free(kw);
	}

	free(line);
	fclose(f);
	return 0;
}

void config_dump(const struct config *cfg)
{
	log_info("config: global enabled=%s", cfg->enabled ? "yes" : "no");
	log_info("config: diag enabled=%s tty=%s socket=%s router=%s",
		 cfg->diag.enabled ? "yes" : "no",
		 cfg->diag.tty ? cfg->diag.tty : "(none)",
		 cfg->diag.socket ? cfg->diag.socket : "(none)",
		 cfg->diag.router ? cfg->diag.router : "(auto)");

	if (cfg->n_ats == 0)
		log_info("config: no AT channels defined");
	for (size_t i = 0; i < cfg->n_ats; i++) {
		const struct at_map *at = &cfg->ats[i];

		log_info("config: at[%zu] %s enabled=%s smd=%s tty=%s",
			 i, at->name ? at->name : "(unnamed)",
			 at->enabled ? "yes" : "no",
			 at->smd ? at->smd : "(none)",
			 at->tty ? at->tty : "(none)");
	}
}

/* =========================== command line =========================== */

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
	int c;

	memset(opts, 0, sizeof(*opts));
	opts->config_path = CONFIG_DEFAULT_PATH;
	*done = false;

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
	if (opts->diag_tty)
		set_str(&cfg->diag.tty, opts->diag_tty);
	if (opts->diag_socket)
		set_str(&cfg->diag.socket, opts->diag_socket);
	if (opts->diag_router)
		set_str(&cfg->diag.router, opts->diag_router);

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
