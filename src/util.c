#include "util.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

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
