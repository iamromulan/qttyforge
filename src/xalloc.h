#ifndef QTTYFORGE_XALLOC_H
#define QTTYFORGE_XALLOC_H

#include <stddef.h>

/*
 * Allocation wrappers that log and abort on failure. Config and argument
 * data are tiny and parsed once at startup; treating OOM there as fatal
 * keeps the call sites free of repetitive NULL checks.
 */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

#endif /* QTTYFORGE_XALLOC_H */
