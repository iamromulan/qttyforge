/* The AT leg: bridge each internal smd channel to a local /dev/ttyATx. */
#include "qttyforge.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int at_start_all(struct engine *e, const struct config *cfg)
{
	int started = 0;

	for (size_t i = 0; i < cfg->n_ats; i++) {
		const struct at_map *at = &cfg->ats[i];
		const char *nm = at->name ? at->name : "at";
		struct pty pty;
		int sfd, fl;

		if (!at->enabled) {
			log_info("at: %s disabled, skipping", nm);
			continue;
		}
		if (!at->smd || !at->tty) {
			log_warn("at: %s missing smd/tty, skipping", nm);
			continue;
		}

		/* Open non-blocking so a dead GLINK channel can't hang startup,
		 * then switch to blocking: the relay uses blocking threads, and
		 * GLINK /dev/smd does not work with O_NONBLOCK + poll(). */
		sfd = open(at->smd, O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (sfd < 0) {
			log_warn("at: %s cannot open %s: %s (skipping)",
				 nm, at->smd, strerror(errno));
			continue;
		}
		fl = fcntl(sfd, F_GETFL);
		if (fl >= 0)
			fcntl(sfd, F_SETFL, fl & ~O_NONBLOCK);

		if (pty_open(&pty, at->tty) < 0) {
			close(sfd);
			continue;
		}

		if (engine_add_relay(e, nm, sfd, pty.master, pty.slave, pty.link) < 0) {
			log_warn("at: %s failed to register relay", nm);
			close(sfd);
			pty_close(&pty);
			continue;
		}

		/* The engine now owns sfd, the pty fds, and the symlink; release
		 * our copy of the path string without removing the symlink. */
		free(pty.link);
		log_info("at: %s  %s <-> %s  ready", nm, at->smd, at->tty);
		started++;
	}

	return started;
}
