/* The relay engine (a buffered, backpressured poll() byte-pump) plus the
 * PTY helper both legs use to create their /dev/tty* endpoints. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE	/* expose openpty() in <util.h> under -std=c11 */
#endif

#include "qttyforge.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
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

#define RELAY_BUF 65536

/* ================================ pty ================================ */

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

/* ============================== engine =============================== */

/* A single-direction byte buffer: valid data is data[off .. len). */
struct ringbuf {
	size_t off;
	size_t len;
	unsigned char data[RELAY_BUF];
};

struct link {
	char *name;
	int fd_a;
	int fd_b;
	int hold_fd;		/* extra fd kept open (e.g. pty slave), or -1 */
	char *unlink_path;	/* symlink to remove on teardown, or NULL */
	bool active;
	struct ringbuf to_a;	/* bytes destined for fd_a (read from fd_b) */
	struct ringbuf to_b;	/* bytes destined for fd_b (read from fd_a) */
};

struct engine {
	struct link **links;
	size_t n;
	size_t cap;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

struct engine *engine_new(void)
{
	struct engine *e = xmalloc(sizeof(*e));

	e->links = NULL;
	e->n = 0;
	e->cap = 0;
	return e;
}

static void link_free(struct link *l)
{
	if (l->fd_a >= 0)
		close(l->fd_a);
	if (l->fd_b >= 0)
		close(l->fd_b);
	if (l->hold_fd >= 0)
		close(l->hold_fd);
	if (l->unlink_path) {
		unlink(l->unlink_path);
		free(l->unlink_path);
	}
	free(l->name);
	free(l);
}

void engine_free(struct engine *e)
{
	if (!e)
		return;
	for (size_t i = 0; i < e->n; i++)
		link_free(e->links[i]);
	free(e->links);
	free(e);
}

size_t engine_count(const struct engine *e)
{
	return e->n;
}

static void set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL);

	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int engine_add_relay(struct engine *e, const char *name, int fd_a, int fd_b,
		     int hold_fd, const char *unlink_path)
{
	struct link *l;

	if (fd_a < 0 || fd_b < 0)
		return -1;

	set_nonblock(fd_a);
	set_nonblock(fd_b);

	l = xmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->name = xstrdup(name ? name : "relay");
	l->fd_a = fd_a;
	l->fd_b = fd_b;
	l->hold_fd = hold_fd;
	l->unlink_path = unlink_path ? xstrdup(unlink_path) : NULL;
	l->active = true;

	if (e->n == e->cap) {
		size_t cap = e->cap ? e->cap * 2 : 4;

		e->links = xrealloc(e->links, cap * sizeof(*e->links));
		e->cap = cap;
	}
	e->links[e->n++] = l;
	return 0;
}

static size_t rb_pending(const struct ringbuf *b)
{
	return b->len - b->off;
}

static void rb_compact(struct ringbuf *b)
{
	if (b->off == 0)
		return;
	if (b->off == b->len) {
		b->off = b->len = 0;
		return;
	}
	memmove(b->data, b->data + b->off, b->len - b->off);
	b->len -= b->off;
	b->off = 0;
}

static size_t rb_room(struct ringbuf *b)
{
	if (b->len == RELAY_BUF && b->off > 0)
		rb_compact(b);
	return RELAY_BUF - b->len;
}

/* Read from fd into dst. Returns false on EOF or a fatal error. */
static bool svc_read(int fd, struct ringbuf *dst)
{
	size_t room = rb_room(dst);
	ssize_t n;

	if (room == 0)
		return true;
	n = read(fd, dst->data + dst->len, room);
	if (n > 0) {
		dst->len += (size_t)n;
		return true;
	}
	if (n == 0)
		return false;	/* EOF */
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return true;
	return false;
}

/* Write pending bytes from src to fd. Returns false on a fatal error. */
static bool svc_write(int fd, struct ringbuf *src)
{
	size_t pend = rb_pending(src);
	ssize_t n;

	if (pend == 0)
		return true;
	n = write(fd, src->data + src->off, pend);
	if (n > 0) {
		src->off += (size_t)n;
		if (src->off == src->len)
			src->off = src->len = 0;
		return true;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return true;
	return false;
}

static void link_drop(struct link *l)
{
	log_warn("relay: %s closed", l->name);
	if (l->fd_a >= 0) {
		close(l->fd_a);
		l->fd_a = -1;
	}
	if (l->fd_b >= 0) {
		close(l->fd_b);
		l->fd_b = -1;
	}
	if (l->hold_fd >= 0) {
		close(l->hold_fd);
		l->hold_fd = -1;
	}
	if (l->unlink_path)
		unlink(l->unlink_path);
	l->active = false;
}

int engine_run(struct engine *e)
{
	struct sigaction sa;
	struct pollfd *pfds;

	if (e->n == 0)
		return 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	pfds = xmalloc(2 * e->n * sizeof(*pfds));

	while (!g_stop) {
		size_t active = 0;

		for (size_t i = 0; i < e->n; i++) {
			struct link *l = e->links[i];
			struct pollfd *pa = &pfds[2 * i];
			struct pollfd *pb = &pfds[2 * i + 1];

			pa->fd = pb->fd = -1;	/* poll() skips negative fds */
			pa->events = pb->events = 0;
			pa->revents = pb->revents = 0;
			if (!l->active)
				continue;
			active++;
			pa->fd = l->fd_a;
			pa->events = (rb_room(&l->to_b) ? POLLIN : 0) |
				     (rb_pending(&l->to_a) ? POLLOUT : 0);
			pb->fd = l->fd_b;
			pb->events = (rb_room(&l->to_a) ? POLLIN : 0) |
				     (rb_pending(&l->to_b) ? POLLOUT : 0);
		}

		if (active == 0) {
			log_info("relay: all channels closed; stopping");
			break;
		}

		int ret = poll(pfds, 2 * e->n, 1000);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			log_err("relay: poll: %s", strerror(errno));
			break;
		}
		if (ret == 0)
			continue;

		for (size_t i = 0; i < e->n; i++) {
			struct link *l = e->links[i];
			struct pollfd *pa = &pfds[2 * i];
			struct pollfd *pb = &pfds[2 * i + 1];
			bool ok = true;

			if (!l->active)
				continue;

			if (pa->revents & POLLIN)
				ok = svc_read(l->fd_a, &l->to_b) && ok;
			if (pb->revents & POLLOUT)
				ok = svc_write(l->fd_b, &l->to_b) && ok;
			if (pb->revents & POLLIN)
				ok = svc_read(l->fd_b, &l->to_a) && ok;
			if (pa->revents & POLLOUT)
				ok = svc_write(l->fd_a, &l->to_a) && ok;

			if ((pa->revents | pb->revents) & (POLLERR | POLLHUP | POLLNVAL))
				ok = false;

			if (!ok)
				link_drop(l);
		}
	}

	free(pfds);
	return 0;
}
