/* The DIAG leg.
 *
 * diag-router is a proprietary prebuilt launched by the service manager WITHOUT
 * -s, and only one instance can bind QRTR DIAG. To get DIAG -> PTY we make the
 * *managed* instance relaunch with `-s addr:port` (additive: USB FunctionFS
 * stays up) via a runtime init override - never editing its unit on disk or
 * moving its binary. qttyforge listens on addr:port, diag-router connects to
 * it, and we relay that socket to /dev/ttyDiag over the engine.
 *
 * Fail-safe: anything missing (no diag-router, unsupported init, no connect)
 * -> warn and skip DIAG; the AT legs and the daemon carry on. The interject is
 * reverted on shutdown (and /run is tmpfs, so it self-clears on reboot too).
 *
 * v1: systemd backend only (procd/sysv stubbed); single accept, no reconnect.
 */
#include "qttyforge.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYSTEMD_DROPIN_DIR "/run/systemd/system/diag-router.service.d"
#define SYSTEMD_DROPIN     SYSTEMD_DROPIN_DIR "/override.conf"
#define DIAG_UNIT          "diag-router.service"
#define ACCEPT_TIMEOUT_MS  12000

static struct {
	int   active;		/* an interject is applied (needs revert) */
	char *router;		/* resolved diag-router path */
} g_diag;

/* fork+exec a command, silencing its output; returns exit code or -1. */
static int run_cmd(char *const argv[])
{
	pid_t pid = fork();
	int st;

	if (pid < 0)
		return -1;
	if (pid == 0) {
		int n = open("/dev/null", O_WRONLY);

		if (n >= 0) {
			dup2(n, STDOUT_FILENO);
			dup2(n, STDERR_FILENO);
			if (n > 2)
				close(n);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* Split "addr:port" (port is the last colon-field). */
static int parse_addr_port(const char *s, char *addr, size_t alen, int *port)
{
	const char *colon = strrchr(s, ':');
	size_t hl;

	if (!colon)
		return -1;
	hl = (size_t)(colon - s);
	if (hl == 0 || hl >= alen)
		return -1;
	memcpy(addr, s, hl);
	addr[hl] = '\0';
	*port = atoi(colon + 1);
	if (*port <= 0 || *port > 65535)
		return -1;
	return 0;
}

static const char *detect_init(void)
{
	char buf[64] = {0};
	FILE *f = fopen("/proc/1/comm", "r");

	if (f) {
		if (fgets(buf, sizeof(buf), f))
			buf[strcspn(buf, "\n")] = '\0';
		fclose(f);
	}
	if (!strcmp(buf, "systemd"))
		return "systemd";
	if (!strcmp(buf, "procd"))
		return "procd";
	if (!strcmp(buf, "init"))
		return "sysv";
	return "unknown";
}

/* config override -> running diag-router's exe -> default path. */
static char *find_diag_router(const struct config *cfg)
{
	DIR *d;
	struct dirent *de;

	if (cfg->diag.router)
		return xstrdup(cfg->diag.router);

	d = opendir("/proc");
	if (d) {
		while ((de = readdir(d))) {
			char p[300], comm[64] = {0}, exe[512];
			FILE *f;
			ssize_t n;

			if (de->d_name[0] < '0' || de->d_name[0] > '9')
				continue;
			snprintf(p, sizeof(p), "/proc/%s/comm", de->d_name);
			f = fopen(p, "r");
			if (!f)
				continue;
			if (fgets(comm, sizeof(comm), f))
				comm[strcspn(comm, "\n")] = '\0';
			fclose(f);
			if (strcmp(comm, "diag-router"))
				continue;
			snprintf(p, sizeof(p), "/proc/%s/exe", de->d_name);
			n = readlink(p, exe, sizeof(exe) - 1);
			if (n > 0) {
				exe[n] = '\0';
				closedir(d);
				return xstrdup(exe);
			}
		}
		closedir(d);
	}
	if (access("/usr/bin/diag-router", X_OK) == 0)
		return xstrdup("/usr/bin/diag-router");
	return NULL;
}

static int make_listener(const char *addr, int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int opt = 1;
	struct sockaddr_in sa;

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
		log_err("diag: bad address '%s'", addr);
		close(fd);
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		log_err("diag: bind %s:%d: %s", addr, port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 1) < 0) {
		log_err("diag: listen: %s", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int systemd_interject(const char *router, const char *addr, int port)
{
	char *reload[] = { "systemctl", "daemon-reload", NULL };
	char *restart[] = { "systemctl", "restart", DIAG_UNIT, NULL };
	FILE *f;

	mkdir("/run/systemd", 0755);
	mkdir("/run/systemd/system", 0755);
	if (mkdir(SYSTEMD_DROPIN_DIR, 0755) < 0 && errno != EEXIST) {
		log_err("diag: mkdir %s: %s", SYSTEMD_DROPIN_DIR, strerror(errno));
		return -1;
	}
	f = fopen(SYSTEMD_DROPIN, "w");
	if (!f) {
		log_err("diag: open %s: %s", SYSTEMD_DROPIN, strerror(errno));
		return -1;
	}
	/* reset the original ExecStart, then relaunch with -s */
	fprintf(f, "[Service]\nExecStart=\nExecStart=%s -s %s:%d\n", router, addr, port);
	fclose(f);

	run_cmd(reload);
	if (run_cmd(restart) != 0) {
		log_err("diag: '%s' restart failed (is it a systemd unit?)", DIAG_UNIT);
		return -1;
	}
	return 0;
}

static void systemd_revert(void)
{
	char *reload[] = { "systemctl", "daemon-reload", NULL };
	char *restart[] = { "systemctl", "restart", DIAG_UNIT, NULL };

	unlink(SYSTEMD_DROPIN);
	rmdir(SYSTEMD_DROPIN_DIR);
	run_cmd(reload);
	run_cmd(restart);
}

int diag_start(struct engine *e, const struct config *cfg)
{
	char addr[64];
	int port, lfd, tfd;
	const char *init;
	char *router;
	struct pty pty;

	if (parse_addr_port(cfg->diag.socket, addr, sizeof(addr), &port) != 0) {
		log_warn("diag: bad socket '%s'; skipping DIAG", cfg->diag.socket);
		return -1;
	}

	init = detect_init();
	if (strcmp(init, "systemd") != 0) {
		log_warn("diag: '%s' interject not implemented yet; skipping DIAG (AT legs unaffected)", init);
		return -1;
	}

	router = find_diag_router(cfg);
	if (!router) {
		log_warn("diag-router not found: could not create ttyDiag (continuing without DIAG)");
		return -1;
	}

	/* listen BEFORE the interject so diag-router -s connects on restart */
	lfd = make_listener(addr, port);
	if (lfd < 0) {
		free(router);
		return -1;
	}

	log_info("diag: interjecting %s -> -s %s:%d (systemd)", router, addr, port);
	if (systemd_interject(router, addr, port) != 0) {
		systemd_revert();
		close(lfd);
		free(router);
		return -1;
	}
	g_diag.active = 1;
	g_diag.router = router;

	log_info("diag: waiting for diag-router to connect...");
	tfd = -1;
	{
		struct pollfd pfd = { .fd = lfd, .events = POLLIN };

		if (poll(&pfd, 1, ACCEPT_TIMEOUT_MS) > 0)
			tfd = accept(lfd, NULL, NULL);
	}
	close(lfd);
	if (tfd < 0) {
		log_warn("diag: diag-router did not connect in %ds; reverting, skipping DIAG",
			 ACCEPT_TIMEOUT_MS / 1000);
		diag_stop();
		return -1;
	}

	if (pty_open(&pty, cfg->diag.tty) < 0) {
		close(tfd);
		diag_stop();
		return -1;
	}
	if (engine_add_relay(e, "diag", tfd, pty.master, pty.slave, pty.link) < 0) {
		close(tfd);
		pty_close(&pty);
		diag_stop();
		return -1;
	}
	free(pty.link);
	log_info("diag: %s <-> diag-router ready", cfg->diag.tty);
	return 0;
}

void diag_stop(void)
{
	if (!g_diag.active)
		return;
	g_diag.active = 0;
	log_info("diag: reverting interject (restoring stock diag-router)");
	systemd_revert();
	free(g_diag.router);
	g_diag.router = NULL;
}
