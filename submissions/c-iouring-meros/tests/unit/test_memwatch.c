#include "check.h"
#include "memwatch.h"

int main(void)
{
	const uint64_t total = 2000000; /* kB: a 2 GB machine, so shedding spans 500,000 down to 250,000 kB available */

	/* A quarter of the RAM or more is available: nothing is shed. */
	CHECK(memwatch_shed_for(2000000, total) == 0);
	CHECK(memwatch_shed_for(562 * 1024, total) == 0); /* the least that was free in a 175,000-user simulation */
	CHECK(memwatch_shed_for(500000, total) == 0);
	/* From a quarter down to an eighth: 0 up to all of them, linearly. */
	CHECK(memwatch_shed_for(499999, total) <= 1);
	CHECK(memwatch_shed_for(375000, total) >= 127 && memwatch_shed_for(375000, total) <= 129);
	CHECK(memwatch_shed_for(250001, total) >= MEMWATCH_SHED_ALL - 1);
	CHECK(memwatch_shed_for(250000, total) == MEMWATCH_SHED_ALL);
	CHECK(memwatch_shed_for(0, total) == MEMWATCH_SHED_ALL);
	/* Never decreasing as memory shrinks. */
	unsigned previous = 0;
	for (uint64_t available = total; available > 0; available -= 1000) {
		unsigned level = memwatch_shed_for(available, total);
		CHECK(level >= previous);
		previous = level;
	}

	/* The shed responses are spread evenly: out of 1,024 responses at level 64, exactly 256 are shed. */
	memwatch watch;
	memwatch_init(&watch);
	watch.shed = 64;
	int shed = 0;
	for (int i = 0; i < 1024; i++)
		shed += memwatch_should_shed(&watch);
	CHECK(shed == 256);
	watch.shed = 0;
	shed = 0;
	for (int i = 0; i < 1024; i++)
		shed += memwatch_should_shed(&watch);
	CHECK(shed == 0);
	watch.shed = MEMWATCH_SHED_ALL;
	shed = 0;
	for (int i = 0; i < 1024; i++)
		shed += memwatch_should_shed(&watch);
	CHECK(shed == 1024);

	/* On a real machine /proc/meminfo is readable and the first check yields a level. */
	memwatch_init(&watch);
	memory_level level = memwatch_check(&watch, 1000);
	CHECK(level == MEMORY_OK || level == MEMORY_LOW || level == MEMORY_CRITICAL);
	CHECK(watch.last_check == 1000);
	memwatch_check(&watch, 1000); /* within the interval: no re-read */
	CHECK(watch.last_check == 1000);
	TEST_MAIN_END();
}
