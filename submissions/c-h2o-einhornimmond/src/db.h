/* SQLite, in gradido-unicorn's arrangement: every loop thread reads on a connection of its own, and
 * one writer thread owns the only write connection and is fed through a queue. */
#ifndef DB_H
#define DB_H

#include <stdint.h>

#include "arnm/json_writer.h"

/* --- reads: on the loop thread that asks ------------------------------------------------- */

typedef struct db_reader db_reader;

/** Opens this thread's read connection and prepares its statements. Exits on failure. */
db_reader *db_reader_open(const char *path);

/*
 * Both add to the document @p w is building and report in @p text_bytes how many bytes of
 * strings they put there, which is what the caller sizes the rendered text by. The strings are
 * borrowed from the reader: the document has to be written before the reader is asked again.
 */

/* Values in a document, counted the way arnm/json_writer.h counts them for a hint: the root,
 * two per scalar member, and one more for every container. */
#define DB_POST_VALUES (1u + 2u + 5u * 2u)                 /* {"post":{five members}} */
#define DB_FEED_VALUES (1u + 2u + 20u * (1u + 5u * 2u))   /* {"posts":[20 x {five members}]} */

/** Adds `"post":{...}`. 1 found, 0 no such post, -1 database error. */
int db_read_post(db_reader *r, int64_t id, arnm_json_writer *w, uint32_t *text_bytes);

/** Adds `"posts":[...]`. 0, or -1 on a database error. */
int db_read_feed(db_reader *r, arnm_json_writer *w, uint32_t *text_bytes);

/** `SELECT 1`. 0 when it answered; otherwise -1 and @p err names why. */
int db_ping(db_reader *r, const char **err);

/* --- writes: on the writer thread -------------------------------------------------------- */

enum { JOB_CREATE = 1, JOB_LIKE = 2 };

/* 500 characters of at most 4 bytes each. */
#define DB_BODY_MAX 2000

typedef struct db_job {
    struct db_job *next;
    int kind;
    int64_t user_id;
    int64_t post_id; /* in: the post to like. out: the id of a created post */
    uint32_t body_len;
    /* Borrowed: whoever submits the job keeps these bytes in place until `done` ran. */
    const char *body;
    /* out */
    int status; /* 201, 200, 404 or 500 */
    uint32_t created_len;
    char created_at[32];
} db_job;

/** Called on the writer thread for every job, after the transaction that holds it committed
 *  (or failed, which the job's status then says). */
typedef void (*db_done_fn)(db_job *job);

/** Opens the write connection and starts the writer thread. Exits on failure. */
void db_writer_start(const char *path, db_done_fn done);

/** Queues @p job. Safe from any thread. The job belongs to the writer until `done` ran. */
void db_writer_submit(db_job *job);

#endif
