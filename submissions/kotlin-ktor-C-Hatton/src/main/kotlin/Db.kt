package server

private const val POST_SELECT = """
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id"""

/** One SQLite connection and its prepared statements. Used by one thread only. */
class Db(path: String) {
    private val conn = SqliteConnection(path).apply {
        busyTimeout(5000)
        exec("PRAGMA journal_mode = WAL")
        exec("PRAGMA synchronous = NORMAL")
        exec("PRAGMA cache_size = -65536") // 64 MiB page cache
        exec("PRAGMA temp_store = MEMORY")
        // No mmap: every checkpoint that grows the file makes SQLite remap it, and the page faults
        // that follow cost more than reading through the page cache (about 20% throughput here).
        exec("PRAGMA mmap_size = 0")
    }
    private val feedStmt = conn.prepare("$POST_SELECT ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    private val postStmt = conn.prepare("$POST_SELECT WHERE p.id = ?")
    private val insertPost = conn.prepare("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
    // Inserts only if the post exists; "inserted nothing" is then either a repeat like or a missing post.
    private val insertLike = conn.prepare(
        "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) " +
            "ON CONFLICT (user_id, post_id) DO NOTHING"
    )
    private val postExists = conn.prepare("SELECT 1 FROM posts WHERE id = ?")
    private val ping = conn.prepare("SELECT 1")
    private val scratch = Scratch()

    fun feed(out: JsonOut): ByteArray {
        out.reset()
        out.raw("{\"posts\":[")
        try {
            var first = true
            while (feedStmt.step()) {
                if (!first) out.byte(','.code)
                first = false
                writePost(feedStmt, out)
            }
        } finally {
            feedStmt.reset()
        }
        out.raw("]}")
        return out.toByteArray()
    }

    fun post(id: Long, out: JsonOut): ByteArray? {
        try {
            postStmt.bind(1, id)
            if (!postStmt.step()) return null
            out.reset()
            out.raw("{\"post\":")
            writePost(postStmt, out)
            out.byte('}'.code)
            return out.toByteArray()
        } finally {
            postStmt.reset()
        }
    }

    private fun writePost(st: SqliteStatement, out: JsonOut) {
        // TEXT columns are copied out as their raw UTF-8 bytes and escaped straight into the output.
        out.raw("{\"id\":"); out.num(st.long(0))
        out.raw(",\"body\":"); text(st, 1, out)
        out.raw(",\"created_at\":"); text(st, 2, out)
        out.raw(",\"author\":"); text(st, 3, out)
        out.raw(",\"like_count\":"); out.num(st.long(4))
        out.byte('}'.code)
    }

    private fun text(st: SqliteStatement, col: Int, out: JsonOut) {
        val n = st.text(col, scratch)
        out.strBytes(scratch.bytes, n)
    }

    fun create(userId: Long, username: String, body: String, out: JsonOut): ByteArray {
        try {
            insertPost.bind(1, userId)
            insertPost.bindText(2, body.toByteArray(Charsets.UTF_8))
            check(insertPost.step()) { "INSERT ... RETURNING returned no row" }
            out.reset()
            out.raw("{\"post\":{\"id\":"); out.num(insertPost.long(0))
            out.raw(",\"body\":"); out.str(body)
            out.raw(",\"created_at\":"); text(insertPost, 1, out)
            out.raw(",\"author\":"); out.str(username)
            out.raw(",\"like_count\":0}}")
            // Step to SQLITE_DONE: that is when the INSERT finishes and, in autocommit mode, commits.
            while (insertPost.step()) continue
        } finally {
            insertPost.reset()
        }
        return out.toByteArray()
    }

    /**
     * Autocommit: the like is committed (WAL, synchronous=NORMAL) before this returns. This connection is
     * the only writer, so its automatic checkpoints always finish and the WAL is rewound instead of growing.
     */
    fun like(userId: Long, postId: Long): Int {
        val inserted = try {
            insertLike.bind(1, userId)
            insertLike.bind(2, postId)
            insertLike.step()
            conn.changes() == 1
        } finally {
            insertLike.reset()
        }
        if (inserted) return LIKED
        try {
            postExists.bind(1, postId)
            return if (postExists.step()) ALREADY else MISSING
        } finally {
            postExists.reset()
        }
    }

    fun ping() {
        try {
            check(ping.step())
        } finally {
            ping.reset()
        }
    }

    companion object {
        const val LIKED = 0
        const val ALREADY = 1
        const val MISSING = 2
    }
}
