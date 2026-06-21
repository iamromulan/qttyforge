/* The relay engine + the PTY helper both legs use for their /dev/tty* ends.
 *
 * One BLOCKING thread per direction per channel. GLINK /dev/smd char devices
 * do not honour poll()/O_NONBLOCK (a poll relay reads 0 bytes on real hardware
 * - see the smd-needs-blocking-io finding), so we use blocking I/O like
 * socat-at-bridge's cats and qcseriald's pthreads. We keep only that good part
 * and fix socat-at-bridge's flaws:
 *   - write-complete retry          (no short-write data loss)
 *   - blocking write = backpressure (no unbounded buffering / "bog up")
 *   - one process, threads joined    (no straggler procs, no respawn races)
 *   - self-cleaning teardown         (close fds, remove the pty symlink)
 *   - sole owner of the smd fd       (no multi-reader contention / stale data)
 * The relay stays dumb (no AT parsing); clients serialise via flock on the PTY.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE	/* expose openpty() in <util.h> under -std=c11 */
#endif

#include "qttyforge.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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

#define RELAY_BUF 16384

/* ================================ pty ================================ */

int pty_open(struct pty *p, const char *link_path)
{
	int master, slave;
	char name[256];
	struct termios t;

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
	/* master stays BLOCKING: the relay uses blocking threads, not poll(). */

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

struct link {
	char *name;
	int fd_a;		/* e.g. smd */
	int fd_b;		/* pty master */
	int hold_fd;		/* pty slave kept open so master never HUPs, or -1 */
	char *unlink_path;	/* pty symlink to remove on teardown, or NULL */
	pthread_t th_ab;	/* fd_a -> fd_b */
	pthread_t th_ba;	/* fd_b -> fd_a */
	pthread_mutex_t lock;
	int dead;
};

struct engine {
	struct link **links;
	size_t n;
	size_t cap;
};

static volatile sig_atomic_t g_stop;
static pthread_t g_main;
static int g_active;	/* live links; atomic ops only */

static void on_stop(int sig)
{
	(void)sig;
	g_stop = 1;
	pthread_kill(g_main, SIGUSR1);	/* wake main out of pause() */
}

static void on_wake(int sig)
{
	(void)sig;	/* no-op: only here to interrupt blocking syscalls (EINTR) */
}

struct engine *engine_new(void)
{
	struct engine *e = xmalloc(sizeof(*e));

	e->links = NULL;
	e->n = 0;
	e->cap = 0;
	return e;
}

static void link_destroy(struct link *l)
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
	pthread_mutex_destroy(&l->lock);
	free(l->name);
	free(l);
}

void engine_free(struct engine *e)
{
	if (!e)
		return;
	for (size_t i = 0; i < e->n; i++)
		link_destroy(e->links[i]);
	free(e->links);
	free(e);
}

size_t engine_count(const struct engine *e)
{
	return e->n;
}

int engine_add_relay(struct engine *e, const char *name, int fd_a, int fd_b,
		     int hold_fd, const char *unlink_path)
{
	struct link *l;

	if (fd_a < 0 || fd_b < 0)
		return -1;

	l = xmalloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->name = xstrdup(name ? name : "relay");
	l->fd_a = fd_a;
	l->fd_b = fd_b;
	l->hold_fd = hold_fd;
	l->unlink_path = unlink_path ? xstrdup(unlink_path) : NULL;
	l->dead = 0;
	pthread_mutex_init(&l->lock, NULL);

	if (e->n == e->cap) {
		size_t cap = e->cap ? e->cap * 2 : 4;

		e->links = xrealloc(e->links, cap * sizeof(*e->links));
		e->cap = cap;
	}
	e->links[e->n++] = l;
	return 0;
}

/* Mark a link dead once: remove its symlink, close its fds (so the channel is
 * released), and wake the sibling thread so it exits too. Self-cleaning. */
static void link_teardown(struct link *l)
{
	pthread_mutex_lock(&l->lock);
	if (!l->dead) {
		l->dead = 1;
		log_warn("relay: %s closed", l->name);
		if (l->unlink_path)
			unlink(l->unlink_path);
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
		pthread_kill(l->th_ab, SIGUSR1);
		pthread_kill(l->th_ba, SIGUSR1);
		/* last link down -> stop the daemon */
		if (__atomic_sub_fetch(&g_active, 1, __ATOMIC_SEQ_CST) == 0) {
			g_stop = 1;
			pthread_kill(g_main, SIGUSR1);
		}
	}
	pthread_mutex_unlock(&l->lock);
}

struct pumparg {
	struct link *l;
	int src;
	int dst;
};

/* Blocking copy src -> dst with write-complete retry. */
static void *pump(void *arg)
{
	struct pumparg *p = arg;
	struct link *l = p->l;
	int src = p->src, dst = p->dst;
	unsigned char buf[RELAY_BUF];
	sigset_t u;

	free(p);

	/* allow SIGUSR1 to interrupt our blocking read()/write() */
	sigemptyset(&u);
	sigaddset(&u, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &u, NULL);

	for (;;) {
		ssize_t n = read(src, buf, sizeof(buf));
		size_t off;

		if (n < 0) {
			if (errno == EINTR) {
				if (g_stop || l->dead)
					break;
				continue;
			}
			break;	/* read error */
		}
		if (n == 0)
			break;	/* EOF */

		for (off = 0; off < (size_t)n; ) {
			ssize_t w = write(dst, buf + off, (size_t)n - off);

			if (w < 0) {
				if (errno == EINTR) {
					if (g_stop || l->dead)
						goto out;
					continue;	/* retry same chunk */
				}
				goto out;	/* write error */
			}
			off += (size_t)w;	/* write-complete retry */
		}
	}
out:
	link_teardown(l);
	return NULL;
}

int engine_run(struct engine *e)
{
	struct sigaction sa;

	if (e->n == 0)
		return 0;

	g_main = pthread_self();
	g_active = (int)e->n;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_wake;		/* no SA_RESTART -> syscalls get EINTR */
	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = on_stop;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	for (size_t i = 0; i < e->n; i++) {
		struct link *l = e->links[i];
		struct pumparg *ab = xmalloc(sizeof(*ab));
		struct pumparg *ba = xmalloc(sizeof(*ba));

		ab->l = l; ab->src = l->fd_a; ab->dst = l->fd_b;
		ba->l = l; ba->src = l->fd_b; ba->dst = l->fd_a;

		/* hold the lock so a fast-failing thread can't tear down (and
		 * pthread_kill the sibling) before both thread ids exist. */
		pthread_mutex_lock(&l->lock);
		pthread_create(&l->th_ba, NULL, pump, ba);
		pthread_create(&l->th_ab, NULL, pump, ab);
		pthread_mutex_unlock(&l->lock);
	}

	while (!g_stop)
		pause();

	log_info("relay: stopping");
	for (size_t i = 0; i < e->n; i++) {
		pthread_kill(e->links[i]->th_ab, SIGUSR1);
		pthread_kill(e->links[i]->th_ba, SIGUSR1);
	}
	for (size_t i = 0; i < e->n; i++) {
		pthread_join(e->links[i]->th_ab, NULL);
		pthread_join(e->links[i]->th_ba, NULL);
	}
	return 0;
}
