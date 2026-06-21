#include "qttyforge.h"

#include <errno.h>
#include <string.h>

static int run(const struct config *cfg)
{
	struct engine *e;
	int n_at, n_diag = 0, rc;

	if (!cfg->enabled) {
		log_info("globally disabled (enabled=0); nothing to do");
		return 0;
	}

	e = engine_new();
	n_at = at_start_all(e, cfg);

	if (cfg->diag.enabled && diag_start(e, cfg) == 0)
		n_diag = 1;

	if (n_at + n_diag <= 0) {
		log_err("nothing to bring up: no AT channels and no DIAG");
		engine_free(e);
		diag_stop();
		return 3;
	}

	log_info("%d AT + %d DIAG channel(s) up; relaying (SIGINT/SIGTERM to stop)",
		 n_at, n_diag);
	rc = engine_run(e);
	engine_free(e);
	diag_stop();
	return rc;
}

int main(int argc, char **argv)
{
	struct cli_opts opts;
	struct config cfg;
	bool done = false;
	int rc;

	if (cli_parse(argc, argv, &opts, &done) != 0)
		return 2;
	if (done)
		return 0;

	log_init(opts.debug);
	log_info("%s %s starting", QTF_NAME, QTF_VERSION);

	config_init(&cfg);

	if (opts.config_path) {
		if (config_load_file(&cfg, opts.config_path) != 0) {
			if (opts.config_explicit) {
				log_err("cannot read config '%s': %s",
					opts.config_path, strerror(errno));
				config_free(&cfg);
				cli_opts_free(&opts);
				return 1;
			}
			log_info("no config at %s; using built-in defaults",
				 opts.config_path);
		}
	} else {
		log_info("config file disabled (--no-config); defaults + args only");
	}

	cli_apply(&cfg, &opts);
	config_dump(&cfg);

	rc = run(&cfg);

	config_free(&cfg);
	cli_opts_free(&opts);
	return rc;
}
