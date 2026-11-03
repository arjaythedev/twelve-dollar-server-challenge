/* A minimal test harness: CHECK records a failure and carries on, main returns the failure count. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures;

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
			check_failures++; \
		} \
	} while (0)

#define CHECK_VIEW(view, literal) \
	CHECK((view).len == sizeof(literal) - 1 && memcmp((view).ptr, literal, sizeof(literal) - 1) == 0)

#define TEST_MAIN_END() \
	do { \
		if (check_failures != 0) \
			fprintf(stderr, "%d check(s) failed\n", check_failures); \
		else \
			puts("ok"); \
		return check_failures != 0; \
	} while (0)

#endif
