/* Small shared helpers: logging + allocation wrappers. */
#include "qttyforge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool debug_enabled;

void log_init(bool debug)
{
	debug_enabled = debug;
}

void log_msg(enum log_level level, const char *fmt, ...)
{
	const char *tag;
	va_list ap;

	if (level == LOG_DBG && !debug_enabled)
		return;

	switch (level) {
	case LOG_ERR:  tag = "error"; break;
	case LOG_WARN: tag = "warn";  break;
	case LOG_INFO: tag = "info";  break;
	case LOG_DBG:  tag = "debug"; break;
	default:       tag = "?";     break;
	}

	fprintf(stderr, "qttyforge: %s: ", tag);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n);

	if (!p) {
		log_err("out of memory (malloc %zu)", n);
		exit(1);
	}
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n);

	if (!q) {
		log_err("out of memory (realloc %zu)", n);
		exit(1);
	}
	return q;
}

char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);

	memcpy(p, s, n);
	return p;
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);

	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}
