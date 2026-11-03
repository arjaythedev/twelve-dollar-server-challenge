#include "db.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "arnm/arena.h"

static void die(sqlite3 *db, const char *what)
{
    fprintf(stderr, "sqlite: %s: %s\n", what, db != NULL ? sqlite3_errmsg(db) : "?");
    exit(1);
}

static sqlite3_stmt *prep(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &stmt, NULL) != SQLITE_OK)
        die(db, sql);
    return stmt;
}

static void exec(sqlite3 *db, const char *sql)
{
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK)
        die(db, sql);
}

/* --- reads ------------------------------------------------------------------------------- */

#define FEED_N 20
/* The widest id range the three-scan feed is used for; see db_read_feed. */
#define FEED_SPAN 256
#define FEED_ROWS_ARENA (64u * 1024u)

/* A string that came out of the database and so may hold a quote or a control character. */
#define FROM_INPUT ARNM_JSON_WRITER_STRING_ESCAPE

#define POST_SELECT                                                                                \
    "SELECT p.id, p.body, p.created_at, u.username, "                                              \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) "                                       \
    "FROM posts p JOIN users u ON u.id = p.user_id "

struct db_reader {
    sqlite3 *db;
    sqlite3_stmt *begin, *commit;
    sqlite3_stmt *post, *feed, *feed_ids, *feed_likes, *feed_rows, *ping;
    /* The strings of the rows of one read, from the step that returns them until the document
     * that borrows them is written; started over with every read. SQLite takes a column's text
     * back at the next step, and the writer copies nothing. Sized for 20 posts of 500 four-byte
     * characters. */
    arnm rows;
};

db_reader *db_reader_open(const char *path)
{
    db_reader *r = calloc(1, sizeof *r);

    if (r == NULL || arnm_init_arena(&r->rows, FEED_ROWS_ARENA) != ARNM_SUCCESS)
        abort();
    if (sqlite3_open_v2(path, &r->db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
        die(r->db, path);
    sqlite3_busy_timeout(r->db, 5000);
    /* SQLite's own page cache, sized for the hot set: the newest posts, their likes, the users. */
    exec(r->db, "PRAGMA cache_size=-131072; PRAGMA temp_store=MEMORY; PRAGMA query_only=ON;");

    r->begin = prep(r->db, "BEGIN");
    r->commit = prep(r->db, "COMMIT");
    r->post = prep(r->db, POST_SELECT "WHERE p.id = ?1");
    r->feed = prep(r->db, POST_SELECT "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    r->feed_ids = prep(r->db, "SELECT id FROM posts ORDER BY created_at DESC, id DESC LIMIT 20");
    r->feed_likes = prep(r->db, "SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2");
    r->feed_rows = prep(r->db, "SELECT p.id, p.body, p.created_at, u.username FROM posts p "
                               "JOIN users u ON u.id = p.user_id WHERE p.id BETWEEN ?1 AND ?2");
    r->ping = prep(r->db, "SELECT 1");
    return r;
}

/* The members of one post object, in the order the spec gives them. */
static void add_post(arnm_json_writer *w, int64_t id, const char *body, size_t body_len,
                     const char *created_at, size_t created_len, const char *author,
                     size_t author_len, int64_t likes, arnm_json_writer_string_flags flags)
{
    arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("id"), (uint64_t)id);
    arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("body"), body, body_len, flags);
    arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("created_at"), created_at,
                                      created_len, flags);
    arnm_json_writer_add_string_flags(w, ARNM_JSON_WRITER_KEY("author"), author, author_len, flags);
    arnm_json_writer_add_uint64(w, ARNM_JSON_WRITER_KEY("like_count"), (uint64_t)likes);
}

/* A string of a row, kept in the reader's arena. */
typedef struct {
    const char *p;
    size_t len;
} text;

static int keep(arnm *arena, sqlite3_stmt *row, int column, text *out)
{
    const int len = sqlite3_column_bytes(row, column);
    uint8_t *copy = NULL;

    out->p = "";
    out->len = 0;
    if (len <= 0)
        return 1;
    if (arnm_clone(&copy, sqlite3_column_text(row, column), (uint32_t)len, arena) != ARNM_SUCCESS)
        return 0;
    out->p = (const char *)copy;
    out->len = (size_t)len;
    return 1;
}

/* The same from a row of (id, body, created_at, username). 0 when the arena is full. */
static int add_post_row(db_reader *r, arnm_json_writer *w, sqlite3_stmt *row, int64_t likes,
                        uint32_t *text_bytes)
{
    text body, created_at, author;

    if (!(keep(&r->rows, row, 1, &body) & keep(&r->rows, row, 2, &created_at) &
          keep(&r->rows, row, 3, &author)))
        return 0;
    add_post(w, sqlite3_column_int64(row, 0), body.p, body.len, created_at.p, created_at.len,
             author.p, author.len, likes, FROM_INPUT);
    *text_bytes += (uint32_t)(body.len + created_at.len + author.len);
    return 1;
}

int db_read_post(db_reader *r, int64_t id, arnm_json_writer *w, uint32_t *text_bytes)
{
    int rc, found = 0;

    arnm_reset(&r->rows);
    sqlite3_bind_int64(r->post, 1, id);
    rc = sqlite3_step(r->post);
    if (rc == SQLITE_ROW) {
        arnm_json_writer_open_object(w, ARNM_JSON_WRITER_KEY("post"));
        found = add_post_row(r, w, r->post, sqlite3_column_int64(r->post, 4), text_bytes) ? 1 : -1;
        arnm_json_writer_close(w);
    }
    sqlite3_reset(r->post);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? found : -1;
}

/* The reference query: one correlated count(*) per post. */
static int feed_reference(db_reader *r, arnm_json_writer *w, uint32_t *text_bytes)
{
    int rc, kept = 1;

    arnm_json_writer_open_array(w, ARNM_JSON_WRITER_KEY("posts"));
    while ((rc = sqlite3_step(r->feed)) == SQLITE_ROW) {
        arnm_json_writer_open_object(w, NULL, 0, false);
        kept &= add_post_row(r, w, r->feed, sqlite3_column_int64(r->feed, 4), text_bytes);
        arnm_json_writer_close(w);
    }
    sqlite3_reset(r->feed);
    arnm_json_writer_close(w);
    return rc == SQLITE_DONE && kept ? 0 : -1;
}

/* The same answer in fewer B-tree descents. Posts are only ever appended, so the 20 newest
 * nearly always have neighbouring ids, and then three range scans replace twenty lookups in
 * each of three trees:
 *
 *   1. the 20 ids, newest first, from the created_at index alone
 *   2. one scan of likes_post_id_idx over [min id, max id], counted per post
 *   3. one rowid range scan of posts, joined to users
 *
 * Step 3 returns the rows in id order, so their strings are kept in the reader's arena and
 * added to the document, borrowed, in the order of step 1. When the ids are spread out, the reference query answers instead.
 * The caller holds a read transaction, so all three see one snapshot. */
static int feed_ranges(db_reader *r, arnm_json_writer *w, uint32_t *text_bytes)
{
    int64_t ids[FEED_N], likes[FEED_N] = {0};
    text body[FEED_N], created_at[FEED_N], author[FEED_N];
    int8_t slot[FEED_SPAN];
    int n = 0, found = 0, kept = 1, rc;

    while (n < FEED_N && (rc = sqlite3_step(r->feed_ids)) == SQLITE_ROW) {
        ids[n++] = sqlite3_column_int64(r->feed_ids, 0);
    }
    sqlite3_reset(r->feed_ids);
    if (n < FEED_N && rc != SQLITE_DONE) return -1;
    if (n == 0) {
        arnm_json_writer_open_array(w, ARNM_JSON_WRITER_KEY("posts"));
        arnm_json_writer_close(w);
        return 0;
    }

    int64_t lo = ids[0], hi = ids[0];
    for (int i = 1; i < n; i++) {
        if (ids[i] < lo) lo = ids[i];
        if (ids[i] > hi) hi = ids[i];
    }
    if (hi - lo >= FEED_SPAN) return feed_reference(r, w, text_bytes);
    memset(slot, -1, (size_t)(hi - lo + 1));
    for (int i = 0; i < n; i++) {
        slot[ids[i] - lo] = (int8_t)i;
    }

    sqlite3_bind_int64(r->feed_likes, 1, lo);
    sqlite3_bind_int64(r->feed_likes, 2, hi);
    while ((rc = sqlite3_step(r->feed_likes)) == SQLITE_ROW) {
        const int i = slot[sqlite3_column_int64(r->feed_likes, 0) - lo];
        if (i >= 0) likes[i]++;
    }
    sqlite3_reset(r->feed_likes);
    if (rc != SQLITE_DONE) return -1;

    sqlite3_bind_int64(r->feed_rows, 1, lo);
    sqlite3_bind_int64(r->feed_rows, 2, hi);
    while ((rc = sqlite3_step(r->feed_rows)) == SQLITE_ROW) {
        const int i = slot[sqlite3_column_int64(r->feed_rows, 0) - lo];
        if (i < 0) continue;
        kept &= keep(&r->rows, r->feed_rows, 1, &body[i]) &
                keep(&r->rows, r->feed_rows, 2, &created_at[i]) &
                keep(&r->rows, r->feed_rows, 3, &author[i])
        ;
        found++;
    }
    sqlite3_reset(r->feed_rows);
    if (rc != SQLITE_DONE || found != n || !kept) return -1;

    arnm_json_writer_open_array(w, ARNM_JSON_WRITER_KEY("posts"));
    for (int i = 0; i < n; i++) {
        arnm_json_writer_open_object(w, NULL, 0, false);
        add_post(w, ids[i], body[i].p, body[i].len, created_at[i].p, created_at[i].len, author[i].p,
                 author[i].len, likes[i], FROM_INPUT);
        arnm_json_writer_close(w);
        *text_bytes += (uint32_t)(body[i].len + created_at[i].len + author[i].len);
    }
    arnm_json_writer_close(w);
    return 0;
}

int db_read_feed(db_reader *r, arnm_json_writer *w, uint32_t *text_bytes)
{
    int rc;

    arnm_reset(&r->rows);
    /* One read transaction around the scans: one snapshot, and one WAL read lock taken and
     * released instead of three. */
    rc = sqlite3_step(r->begin);
    sqlite3_reset(r->begin);
    if (rc != SQLITE_DONE) return -1;
    rc = feed_ranges(r, w, text_bytes);
    sqlite3_step(r->commit);
    sqlite3_reset(r->commit);
    return rc;
}

int db_ping(db_reader *r, const char **err)
{
    const int rc = sqlite3_step(r->ping);

    sqlite3_reset(r->ping);
    if (rc == SQLITE_ROW)
        return 0;
    *err = sqlite3_errmsg(r->db);
    return -1;
}

/* --- writes ------------------------------------------------------------------------------ */

static struct {
    sqlite3 *db;
    sqlite3_stmt *begin, *commit, *rollback, *create, *like, *exists;
    db_done_fn done;
    pthread_mutex_t mu;
    pthread_cond_t cond;
    db_job *head, *tail;
} w = {.mu = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static int step_done(sqlite3_stmt *stmt)
{
    const int rc = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    return rc == SQLITE_DONE;
}

static void run_create(db_job *job)
{
    job->status = 500;
    sqlite3_bind_int64(w.create, 1, job->user_id);
    sqlite3_bind_text(w.create, 2, job->body, (int)job->body_len, SQLITE_STATIC);
    if (sqlite3_step(w.create) == SQLITE_ROW) {
        const unsigned char *at = sqlite3_column_text(w.create, 1);
        const int at_len = sqlite3_column_bytes(w.create, 1);

        job->post_id = sqlite3_column_int64(w.create, 0);
        if (at != NULL && at_len > 0 && (size_t)at_len <= sizeof job->created_at) {
            memcpy(job->created_at, at, (size_t)at_len);
            job->created_len = (uint32_t)at_len;
            /* Stepped to the end so that the row is written, not merely returned. */
            if (sqlite3_step(w.create) == SQLITE_DONE)
                job->status = 201;
        }
    }
    sqlite3_reset(w.create);
}

static void run_like(db_job *job)
{
    job->status = 500;
    sqlite3_bind_int64(w.like, 1, job->user_id);
    sqlite3_bind_int64(w.like, 2, job->post_id);
    if (!step_done(w.like))
        return;
    if (sqlite3_changes(w.db) > 0) {
        job->status = 201;
        return;
    }
    /* Nothing was inserted: the like is already there, or the post is not. */
    sqlite3_bind_int64(w.exists, 1, job->post_id);
    const int rc = sqlite3_step(w.exists);
    sqlite3_reset(w.exists);
    if (rc == SQLITE_ROW)
        job->status = 200;
    else if (rc == SQLITE_DONE)
        job->status = 404;
}

/* Group commit. Whatever queued up while the previous transaction was committing is taken in
 * one piece and written in one transaction, and no job is reported before that transaction
 * committed -- so a 201 is never sent for a row that is not durable. */
static void *writer_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&w.mu);
        while (w.head == NULL)
            pthread_cond_wait(&w.cond, &w.mu);
        db_job *batch = w.head;
        w.head = w.tail = NULL;
        pthread_mutex_unlock(&w.mu);

        int ok = step_done(w.begin);
        if (ok) {
            for (db_job *job = batch; job != NULL; job = job->next) {
                if (job->kind == JOB_CREATE)
                    run_create(job);
                else
                    run_like(job);
            }
            ok = step_done(w.commit);
            if (!ok)
                (void)step_done(w.rollback);
        }
        for (db_job *job = batch, *next; job != NULL; job = next) {
            next = job->next; /* read before `done`, which gives the job back */
            if (!ok)
                job->status = 500;
            w.done(job);
        }
    }
    return NULL;
}

void db_writer_start(const char *path, db_done_fn done)
{
    pthread_t thread;

    w.done = done;
    if (sqlite3_open_v2(path, &w.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK)
        die(w.db, path);
    sqlite3_busy_timeout(w.db, 5000);
    exec(w.db, "PRAGMA journal_mode=WAL;"
               "PRAGMA synchronous=NORMAL;"
               "PRAGMA foreign_keys=ON;"
               "PRAGMA cache_size=-32768;"
               "PRAGMA temp_store=MEMORY;"
               "PRAGMA journal_size_limit=67108864;");
    w.begin = prep(w.db, "BEGIN IMMEDIATE");
    w.commit = prep(w.db, "COMMIT");
    w.rollback = prep(w.db, "ROLLBACK");
    w.create = prep(w.db, "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
    w.like = prep(w.db, "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 "
                        "WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) "
                        "ON CONFLICT (user_id, post_id) DO NOTHING");
    w.exists = prep(w.db, "SELECT 1 FROM posts WHERE id = ?1");
    if (pthread_create(&thread, NULL, writer_main, NULL) != 0) {
        fprintf(stderr, "cannot start the writer thread\n");
        exit(1);
    }
}

void db_writer_submit(db_job *job)
{
    job->next = NULL;
    pthread_mutex_lock(&w.mu);
    if (w.tail != NULL)
        w.tail->next = job;
    else
        w.head = job;
    w.tail = job;
    pthread_cond_signal(&w.cond);
    pthread_mutex_unlock(&w.mu);
}
