#ifndef QTTYFORGE_LOG_H
#define QTTYFORGE_LOG_H

#include <stdbool.h>

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

#endif /* QTTYFORGE_LOG_H */
