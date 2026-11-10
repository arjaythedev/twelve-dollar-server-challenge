package nio

import org.sqlite.SQLiteConfig
import org.sqlite.SQLiteOpenMode
import java.sql.Connection
import java.sql.DriverManager
import java.sql.PreparedStatement

/**
 * Two SQLite connections, both used only by the event-loop thread, with every statement prepared once.
 * - reader: feed / post / health. busy_timeout 5 s (WAL readers practically never wait).
 * - writer: the group-commit transaction. busy_timeout 0, so when another worker process holds the write lock,
 *   BEGIN fails at once and the batch is retried on the next loop pass. A sleeping busy handler would freeze
 *   the whole event loop: measured at 0.5-1.4 s per stall with 2 workers at 120k users.
 * Responses are built as JSON by SQLite itself and fetched as raw UTF-8 bytes (no Strings, no serializer).
 */
class Db(path: String) {
    private val conn: Connection
    private val wconn: Connection

    private val health: PreparedStatement
    private val feed: PreparedStatement
    private val feedIds: PreparedStatement
    private val feedLikes: PreparedStatement
    private val feedRows: PreparedStatement
    private val post: PreparedStatement
    private val insertPost: PreparedStatement
    private val postExists: PreparedStatement
    private val insertLike: PreparedStatement
    private val begin: PreparedStatement
    private val commit: PreparedStatement
    private val rollback: PreparedStatement

    companion object {
        // Post object, exactly in the wire key order.
        private const val POST_JSON =
            "json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username," +
                "'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))"

        private const val FEED_SPAN = 256
        private val FEED_HEAD = "{\"posts\":[".toByteArray()
        private val LIKE_KEY = ",\"like_count\":".toByteArray()

        fun config(busyMs: Int = 5000): SQLiteConfig = SQLiteConfig().apply {
            setJournalMode(SQLiteConfig.JournalMode.WAL)
            setSynchronous(SQLiteConfig.SynchronousMode.NORMAL) // rule 6 minimum: WAL + NORMAL
            setOpenMode(SQLiteOpenMode.NOMUTEX)
            setBusyTimeout(busyMs)
            setTempStore(SQLiteConfig.TempStore.MEMORY)
        }

        private fun tune(c: Connection) = c.createStatement().use { st ->
            // No mmap: pages come through SQLite's own page cache (pread on a miss). Measured LOCAL at 112k users,
            // two interleaved pairs: 50.3/48.9 -> 43.9/45.0 us/req vs mmap_size=512 MB + an 8 MB cache.
            // (Same direction as mojo/pavlyhalim/le0nvfx on the leaderboard; ~64 MiB is their sweet spot.)
            st.execute("PRAGMA mmap_size=0")
            st.execute("PRAGMA cache_size=-65536")       // 64 MiB, only as much as gets touched is allocated
            st.execute("PRAGMA wal_autocheckpoint=0")     // checkpoints run on the Checkpointer thread
            st.execute("PRAGMA journal_size_limit=67108864") // truncate the WAL file back to 64 MB after a reset
        }
    }

    init {
        conn = DriverManager.getConnection("jdbc:sqlite:$path", config().toProperties())
        tune(conn)
        wconn = DriverManager.getConnection("jdbc:sqlite:$path", config(busyMs = 0).toProperties())
        tune(wconn)
        health = conn.prepareStatement("SELECT 1")
        feed = conn.prepareStatement(
            "SELECT '{\"posts\":['||coalesce(group_concat(j,',' ORDER BY ca DESC,id DESC),'')||']}' FROM (" +
                "SELECT p.id id,p.created_at ca,$POST_JSON j FROM posts p JOIN users u ON u.id=p.user_id " +
                "ORDER BY p.created_at DESC,p.id DESC LIMIT 20)"
        )
        // Two-step feed (see feed()). Credit: the range-scan pattern is from zig-iouring-unorig-username
        // (arjaythedev/twelve-dollar-server-challenge, src/main.zig feedRange, MIT).
        feedIds = conn.prepareStatement("SELECT id FROM posts ORDER BY created_at DESC,id DESC LIMIT 20")
        feedLikes = conn.prepareStatement("SELECT post_id FROM likes WHERE post_id BETWEEN ?1 AND ?2")
        feedRows = conn.prepareStatement(
            "SELECT p.id,json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username) " +
                "FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id BETWEEN ?1 AND ?2"
        )
        post = conn.prepareStatement(
            "SELECT '{\"post\":'||$POST_JSON||'}' FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?"
        )
        insertPost = wconn.prepareStatement(
            "INSERT INTO posts(user_id,body) VALUES(?1,?2) RETURNING '{\"post\":'||" +
                "json_object('id',id,'body',body,'created_at',created_at,'author',?3,'like_count',0)||'}'"
        )
        postExists = wconn.prepareStatement("SELECT 1 FROM posts WHERE id=?")
        insertLike = wconn.prepareStatement(
            "INSERT INTO likes(user_id,post_id) VALUES(?,?) ON CONFLICT(user_id,post_id) DO NOTHING"
        )
        begin = wconn.prepareStatement("BEGIN IMMEDIATE")
        commit = wconn.prepareStatement("COMMIT")
        rollback = wconn.prepareStatement("ROLLBACK")
    }

    fun ping() {
        health.executeQuery().use { if (!it.next()) error("SELECT 1 returned no row") }
    }

    private val fIds = LongArray(20)
    private val fSlot = IntArray(FEED_SPAN)
    private val fLikes = IntArray(20)
    private val fRows = arrayOfNulls<ByteArray>(20)
    private val fNum = ByteArray(20)

    /**
     * The 20 newest ids first (index-only), then, when they sit in a narrow id range (they do as soon as anybody
     * posts: new posts get the newest timestamps and the next ids), one range scan of likes_post_id_idx tallied
     * into an array plus one rowid range over posts JOIN users, reordered here. Otherwise (or if a row is missing)
     * the original single query with 20 correlated count(*)s. Same bytes either way: SQLite still builds each
     * post's JSON; only the like_count is appended here.
     */
    fun feed(): ByteArray = feedRange() ?: feed.executeQuery().use { it.next(); it.getBytes(1) }

    private fun feedRange(): ByteArray? {
        var n = 0
        feedIds.executeQuery().use { while (it.next()) fIds[n++] = it.getLong(1) }
        if (n == 0) return null
        var lo = fIds[0]; var hi = fIds[0]
        for (i in 1 until n) { val v = fIds[i]; if (v < lo) lo = v; if (v > hi) hi = v }
        if (hi - lo >= FEED_SPAN) return null
        java.util.Arrays.fill(fSlot, 0, (hi - lo + 1).toInt(), -1)
        for (i in 0 until n) { fSlot[(fIds[i] - lo).toInt()] = i; fLikes[i] = 0; fRows[i] = null }
        feedLikes.setLong(1, lo); feedLikes.setLong(2, hi)
        feedLikes.executeQuery().use { while (it.next()) { val k = fSlot[(it.getLong(1) - lo).toInt()]; if (k >= 0) fLikes[k]++ } }
        var found = 0
        var size = 12 + n * 26  // per post: "," + like key + up to 10 digits + "}" - the dropped "}"
        feedRows.setLong(1, lo); feedRows.setLong(2, hi)
        feedRows.executeQuery().use {
            while (it.next()) {
                val k = fSlot[(it.getLong(1) - lo).toInt()]
                if (k < 0) continue
                val j = it.getBytes(2); fRows[k] = j; size += j.size; found++
            }
        }
        if (found != n) return null
        val out = ByteArray(size)
        var o = put(out, 0, FEED_HEAD)
        for (i in 0 until n) {
            if (i > 0) out[o++] = ','.code.toByte()
            val j = fRows[i]!!
            System.arraycopy(j, 0, out, o, j.size - 1); o += j.size - 1   // drop the closing '}'
            o = put(out, o, LIKE_KEY)
            var v = fLikes[i]; var p = fNum.size
            do { fNum[--p] = ('0'.code + v % 10).toByte(); v /= 10 } while (v > 0)
            System.arraycopy(fNum, p, out, o, fNum.size - p); o += fNum.size - p
            out[o++] = '}'.code.toByte()
        }
        out[o++] = ']'.code.toByte(); out[o++] = '}'.code.toByte()
        return if (o == out.size) out else out.copyOf(o)
    }

    private fun put(dst: ByteArray, at: Int, src: ByteArray): Int { System.arraycopy(src, 0, dst, at, src.size); return at + src.size }

    fun post(id: Long): ByteArray? {
        post.setLong(1, id)
        return post.executeQuery().use { if (it.next()) it.getBytes(1) else null }
    }

    fun insertPost(userId: Long, body: String, author: String): ByteArray {
        insertPost.setLong(1, userId)
        insertPost.setString(2, body)
        insertPost.setString(3, author)
        return insertPost.executeQuery().use { it.next(); it.getBytes(1) }
    }

    /** -1 = no such post, 1 = newly liked, 0 = already liked. */
    fun like(userId: Long, postId: Long): Int {
        postExists.setLong(1, postId)
        if (!postExists.executeQuery().use { it.next() }) return -1
        insertLike.setLong(1, userId)
        insertLike.setLong(2, postId)
        return insertLike.executeUpdate()
    }

    /** false = another process holds the write lock right now (SQLITE_BUSY); try again on the next loop pass. */
    fun tryBegin(): Boolean = try {
        begin.executeUpdate(); true
    } catch (e: java.sql.SQLException) {
        if ((e.errorCode and 0xff) == 5) false else throw e
    }
    fun commit() { commit.executeUpdate() }
    fun rollback() { try { rollback.executeUpdate() } catch (_: Exception) {} }
}

/** Runs WAL checkpoints off the response path, on its own connection. */
class Checkpointer(private val path: String) : Thread("checkpointer") {
    init { isDaemon = true; priority = MIN_PRIORITY }

    override fun run() {
        val c = DriverManager.getConnection("jdbc:sqlite:$path", Db.config().toProperties())
        val st = c.createStatement()
        while (true) {
            try {
                sleep(1000)
                st.execute("PRAGMA wal_checkpoint(PASSIVE)")
            } catch (e: InterruptedException) {
                return
            } catch (e: Exception) {
                System.err.println("checkpoint: $e")
            }
        }
    }
}
