#if defined(__APPLE__)
#define _DARWIN_C_SOURCE	/* expose openpty() in <util.h> under -std=c11 */
#endif

#include "pty.h"
#include "log.h"
#include "xalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

int pty_open(struct pty *p, const char *link_path)
{
	int master, slave;
	char name[256];
	struct termios t;
	int fl;

	p->master = p->slave = -1;
	p->link = NULL;

	if (openpty(&master, &slave, name, NULL, NULL) < 0) {
		log_err("pty: openpty: %s", strerror(errno));
		return -1;
	}

	if (tcgetattr(slave, &t) == 0) {
		cfmakeraw(&t);
		tcsetattr(slave, TCSANOW, &t);
	}

	fl = fcntl(master, F_GETFL);
	if (fl >= 0)
		fcntl(master, F_SETFL, fl | O_NONBLOCK);

	unlink(link_path);
	if (symlink(name, link_path) < 0) {
		log_err("pty: symlink %s -> %s: %s", link_path, name, strerror(errno));
		close(master);
		close(slave);
		return -1;
	}
	chmod(name, 0660);

	p->master = master;
	p->slave = slave;
	p->link = xstrdup(link_path);
	return 0;
}

void pty_close(struct pty *p)
{
	if (p->master >= 0) {
		close(p->master);
		p->master = -1;
	}
	if (p->slave >= 0) {
		close(p->slave);
		p->slave = -1;
	}
	if (p->link) {
		unlink(p->link);
		free(p->link);
		p->link = NULL;
	}
}
