/* config: the server's settings, read from the environment variables the challenge defines. */
#ifndef CONFIG_H
#define CONFIG_H

#include <netinet/in.h>
#include <stdint.h>

#include "strview.h"

typedef struct config {
	const char *database_path; /* SQLITE_PATH */
	strview jwt_secret;        /* JWT_SECRET */
	struct in_addr bind_address; /* HOST, default 0.0.0.0 */
	uint16_t port;             /* PORT, default 3000 */
} config;

/* Exits with a message when a required variable is missing or a value is invalid. */
config config_from_environment(void);

#endif
