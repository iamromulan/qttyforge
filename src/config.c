#include "config.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
	if (cfg->n_ats == cfg->cap_ats) {
		size_t cap = cfg->cap_ats ? cfg->cap_ats * 2 : 4;

		cfg->ats = xrealloc(cfg->ats, cap * sizeof(*cfg->ats));
		cfg->cap_ats = cap;
	}

	struct at_map *at = &cfg->ats[cfg->n_ats++];

	memset(at, 0, sizeof(*at));
	at->name = name ? xstrdup(name) : NULL;
	at->enabled = true;	/* on unless the config says otherwise */
	return at;
}

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

	if (!f)
		return -1;	/* errno set; caller logs */

	char *line = NULL;
	size_t linecap = 0;
	ssize_t len;
	int lineno = 0;
	enum section sect = SECT_NONE;
	struct at_map *cur_at = NULL;

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
