package feed.db;

/** SQL text of every prepared statement. Key order in the JSON objects is the spec's. */
final class Sql {

    private Sql() {
    }

    /** One post as a JSON object; {@code p} is a posts row, {@code u} its author. */
    static final String POST_JSON =
            "json_object('id', p.id, 'body', p.body, 'created_at', p.created_at, 'author', u.username, "
                    + "'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))";

    /**
     * Up to 20 rows, one post object each, newest first; {@code FeedDb.feed} wraps them in {@code {"posts":[...]}}.
     * The plan walks {@code posts_created_at_id_idx} backwards and stops after 20 rows (no sort step), which is
     * cheaper than building the whole body in SQLite with {@code group_concat}.
     */
    static final String FEED =
            "SELECT " + POST_JSON
                    + " FROM posts p JOIN users u ON u.id = p.user_id"
                    + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";

    /** {@code ?1} post id. One row with the whole {@code GET /posts/:id} body, or no row. */
    static final String POST =
            "SELECT '{\"post\":' || " + POST_JSON + " || '}'"
                    + " FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1";

    /** {@code ?1} user id, {@code ?2} body, {@code ?3} author username. One row: the whole 201 body. */
    static final String CREATE =
            "INSERT INTO posts (user_id, body) VALUES (?1, ?2)"
                    + " RETURNING '{\"post\":' || json_object('id', id, 'body', body, 'created_at', created_at,"
                    + " 'author', ?3, 'like_count', 0) || '}'";

    /** {@code ?1} user id, {@code ?2} post id. {@code changes()} is 1 for a new like, else 0. */
    static final String LIKE =
            "INSERT INTO likes (user_id, post_id)"
                    + " SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)"
                    + " ON CONFLICT (user_id, post_id) DO NOTHING";

    /** {@code ?1} post id. One row if the post exists. */
    static final String POST_EXISTS = "SELECT 1 FROM posts WHERE id = ?1";

    static final String HEALTH = "SELECT 1";

    static final String BEGIN = "BEGIN";
    static final String COMMIT = "COMMIT";
    static final String ROLLBACK = "ROLLBACK";

    /** Startup pragmas, in order. {@code locking_mode} must come first, {@code wal_autocheckpoint} before the hook. */
    static final String[] PRAGMAS_AFTER_JOURNAL_MODE = {
            "PRAGMA synchronous = NORMAL",
            "PRAGMA foreign_keys = ON",
            "PRAGMA cache_size = 500",
            "PRAGMA mmap_size = 1073741824",
            "PRAGMA temp_store = MEMORY",
            "PRAGMA journal_size_limit = 67108864",
            "PRAGMA wal_autocheckpoint = 0",
    };

    /** Prewarm queries: touch every table and index once through mmap (PLAN 6.7). */
    static final String[] PREWARM = {
            "SELECT sum(length(body)) FROM posts",
            "SELECT count(*) FROM posts INDEXED BY posts_created_at_id_idx WHERE created_at >= ''",
            "SELECT count(*) FROM posts INDEXED BY posts_user_id_idx WHERE user_id > 0",
            "SELECT count(*) FROM likes INDEXED BY likes_post_id_idx WHERE post_id > 0",
            "SELECT count(*) FROM likes",
            "SELECT sum(length(username)) FROM users",
    };
}
