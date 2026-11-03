/*
 * query_bench: nanoseconds per call of the store's read queries, without any networking.
 *
 *   query_bench <database-copy> [wal_writes]
 *
 * wal_writes > 0 first commits that many posts, so the write-ahead log is not empty: that is the
 * state of a server under write load, and every page read then has to look in the log first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "store.h"

static double now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void ignore_post(const post_row *post, void *context)
{
	*(volatile int64_t *)context += post->id;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: query_bench <database-copy> [wal_writes]\n");
		return 2;
	}
	store *db = store_open(argv[1]);
	int wal_writes = argc > 2 ? atoi(argv[2]) : 0;
	volatile int64_t sink = 0;

	for (int i = 0; i < wal_writes; i++) {
		new_post created;

		store_begin(db);
		store_insert_post(db, 1 + i % 20000, STRVIEW("benchmark post, plain ascii text of a typical length"), &created);
		store_commit(db);
		store_checkpoint_if_due(db); /* the server does this between rounds */
	}

	enum { POST_CALLS = 400000, FEED_CALLS = 40000 };
	int64_t newest = 500000 + wal_writes;
	double start = now_ns();

	for (int i = 0; i < POST_CALLS; i++)
		store_find_post(db, newest - i % 20, ignore_post, (void *)&sink);
	double post_ns = (now_ns() - start) / POST_CALLS;

	start = now_ns();
	for (int i = 0; i < FEED_CALLS; i++)
		store_read_feed(db, ignore_post, (void *)&sink);
	double feed_ns = (now_ns() - start) / FEED_CALLS;

	printf("wal_writes=%d  post=%.0f ns  feed=%.0f ns\n", wal_writes, post_ns, feed_ns);
	store_close(db);
	return 0;
}
