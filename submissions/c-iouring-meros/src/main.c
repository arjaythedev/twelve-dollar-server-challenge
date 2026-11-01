#include "config.h"
#include "server.h"

int main(void)
{
	config cfg = config_from_environment();

	server_run(&cfg);
	return 0;
}
