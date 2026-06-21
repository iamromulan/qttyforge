#include "log.h"

#include <stdarg.h>
#include <stdio.h>

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
