#include "cli.h"
#include "config.h"
#include "log.h"
#include "version.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

int main(int argc, char **argv)
{
	struct cli_opts opts;
	struct config cfg;
	bool done = false;
	int rc = 0;

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

	if (!cfg.enabled) {
		log_info("globally disabled (enabled=0); nothing to do");
	} else {
		size_t enabled_ats = 0;

		for (size_t i = 0; i < cfg.n_ats; i++)
			if (cfg.ats[i].enabled)
				enabled_ats++;

		/*
		 * Nothing to bring up -> abort rather than idle. The DIAG leg's
		 * runtime availability also depends on diag-router being present;
		 * if it is missing the engine downgrades DIAG to a warning and
		 * re-applies this same check (a missing diag-router with no AT
		 * channels is therefore a hard exit, not an idle daemon).
		 */
		if (!cfg.diag.enabled && enabled_ats == 0) {
			log_err("nothing to bring up: DIAG disabled and no enabled AT channels");
			rc = 3;
		} else {
			log_warn("relay engine not yet implemented; would bring up DIAG=%s and %zu AT channel(s)",
				 cfg.diag.enabled ? "on" : "off", enabled_ats);
		}
	}

	config_free(&cfg);
	cli_opts_free(&opts);
	return rc;
}
