#ifndef QTTYFORGE_AT_H
#define QTTYFORGE_AT_H

#include "config.h"
#include "relay.h"

/*
 * Bring up every enabled AT channel in cfg: open its smd device, create a
 * PTY at its tty path, and register a relay on the engine. Failures on an
 * individual channel are logged and skipped (non-fatal). Returns the number
 * of channels successfully started.
 */
int at_start_all(struct engine *e, const struct config *cfg);

#endif /* QTTYFORGE_AT_H */
