#include "log.h"
#include "version.h"

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	log_init(false);
	log_info("%s %s", QTF_NAME, QTF_VERSION);
	log_warn("foundation build: config parser and relay engine not wired yet");
	return 0;
}
