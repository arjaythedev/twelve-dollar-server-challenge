#include "memwatch.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

unsigned memwatch_shed_for(uint64_t available_kb, uint64_t total_kb)
{
	uint64_t quarter = total_kb / 4, eighth = total_kb / 8;

	if (available_kb >= quarter)
		return 0;
	if (available_kb <= eighth)
		return MEMWATCH_SHED_ALL;
	return (unsigned)((quarter - available_kb) * MEMWATCH_SHED_ALL / (quarter - eighth)); /* linear */
}

void memwatch_init(memwatch *watch)
{
	const char *test_path = getenv("SERVER_TEST_MEMINFO");

	memset(watch, 0, sizeof *watch);
	watch->meminfo_path = test_path != NULL ? test_path : "/proc/meminfo";
}

bool memwatch_should_shed(memwatch *watch)
{
	watch->credit += watch->shed;
	if (watch->credit < MEMWATCH_SHED_ALL)
		return false;
	watch->credit -= MEMWATCH_SHED_ALL;
	return true;
}

/* Finds "<name>:   <number> kB" in the text of /proc/meminfo. */
static bool find_kb(const char *text, const char *name, uint64_t *value)
{
	const char *line = strstr(text, name);

	if (line == NULL)
		return false;
	*value = strtoull(line + strlen(name), NULL, 10);
	return true;
}

static const char *level_name(memory_level level)
{
	return level == MEMORY_OK ? "ok" : level == MEMORY_LOW ? "shedding" : "critical";
}

/* One line on stderr when the picture changes noticeably: the level, or the share shed by a quarter or more. */
static void report(memwatch *watch, memory_level before, uint64_t available_kb)
{
	bool moved = watch->shed > watch->reported_shed ? watch->shed - watch->reported_shed >= 64
	                                                : watch->reported_shed - watch->shed >= 64;

	if (watch->level == before && !moved)
		return;
	watch->reported_shed = watch->shed;
	fprintf(stderr, "memwatch: %llu MB available, %s: %u%% of responses close their connection\n",
	        (unsigned long long)(available_kb / 1024), level_name(watch->level), watch->shed * 100 / MEMWATCH_SHED_ALL);
}

memory_level memwatch_check(memwatch *watch, time_t now)
{
	char text[4096];
	uint64_t total_kb, available_kb;

	if (now - watch->last_check < MEMWATCH_INTERVAL_SECONDS)
		return watch->level;
	watch->last_check = now;

	int fd = open(watch->meminfo_path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return watch->level;
	ssize_t length = read(fd, text, sizeof text - 1);

	close(fd);
	if (length <= 0)
		return watch->level;
	text[length] = '\0';
	if (!find_kb(text, "MemTotal:", &total_kb) || !find_kb(text, "MemAvailable:", &available_kb))
		return watch->level;

	memory_level before = watch->level;

	watch->shed = memwatch_shed_for(available_kb, total_kb);
	watch->level = watch->shed >= MEMWATCH_SHED_ALL ? MEMORY_CRITICAL : watch->shed > 0 ? MEMORY_LOW : MEMORY_OK;
	report(watch, before, available_kb);
	return watch->level;
}
