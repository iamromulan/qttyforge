#ifndef QTTYFORGE_PTY_H
#define QTTYFORGE_PTY_H

struct pty {
	int master;
	int slave;
	char *link;	/* symlink path created (heap), or NULL */
};

/*
 * Create a PTY pair, put the slave in raw mode, symlink link_path -> the
 * slave device, and make it accessible. The master is set non-blocking.
 * Returns 0 on success (fields populated), -1 on failure.
 */
int pty_open(struct pty *p, const char *link_path);

/* Full teardown: close both fds, remove the symlink, free link. */
void pty_close(struct pty *p);

#endif /* QTTYFORGE_PTY_H */
