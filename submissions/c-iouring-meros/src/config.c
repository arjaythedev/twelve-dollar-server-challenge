#include "config.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "fatal.h"

enum { DEFAULT_PORT = 3000 };

static const char *require_env(const char *name)
{
	const char *value = getenv(name);

	if (value == NULL || *value == '\0')
		fatal("environment variable %s is required", name);
	return value;
}

config config_from_environment(void)
{
	config cfg;
	const char *host = getenv("HOST");
	const char *port = getenv("PORT");
	const char *secret = require_env("JWT_SECRET");

	cfg.database_path = require_env("SQLITE_PATH");
	cfg.jwt_secret = strview_make(secret, strlen(secret));
	if (inet_pton(AF_INET, host != NULL ? host : "0.0.0.0", &cfg.bind_address) != 1)
		fatal("HOST must be an IPv4 address, got \"%s\"", host);
	char *end = NULL;
	long number = port != NULL ? strtol(port, &end, 10) : DEFAULT_PORT;

	if (number < 1 || number > 65535 || (port != NULL && (end == port || *end != '\0')))
		fatal("PORT must be 1..65535, got \"%s\"", port);
	cfg.port = (uint16_t)number;
	return cfg;
}
