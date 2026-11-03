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

        fun config(busyMs: Int = 5000): SQLiteConfig = SQLiteConfig().apply {
            setJournalMode(SQLiteConfig.JournalMode.WAL)
            setSynchronous(SQLiteConfig.SynchronousMode.NORMAL) // rule 6 minimum: WAL + NORMAL
            setOpenMode(SQLiteOpenMode.NOMUTEX)
            setBusyTimeout(busyMs)
            setTempStore(SQLiteConfig.TempStore.MEMORY)
        }

        private fun tune(c: Connection, cacheKb: Int) = c.createStatement().use { st ->
            st.execute("PRAGMA mmap_size=536870912")     // reads go through the OS page cache
            st.execute("PRAGMA cache_size=-$cacheKb")
            st.execute("PRAGMA wal_autocheckpoint=0")     // checkpoints run on the Checkpointer thread
            st.execute("PRAGMA journal_size_limit=67108864") // truncate the WAL file back to 64 MB after a reset
        }
    }

    init {
        conn = DriverManager.getConnection("jdbc:sqlite:$path", config().toProperties())
        tune(conn, 8000)
        wconn = DriverManager.getConnection("jdbc:sqlite:$path", config(busyMs = 0).toProperties())
        tune(wconn, 2000)
        health = conn.prepareStatement("SELECT 1")
        feed = conn.prepareStatement(
            "SELECT '{\"posts\":['||coalesce(group_concat(j,',' ORDER BY ca DESC,id DESC),'')||']}' FROM (" +
                "SELECT p.id id,p.created_at ca,$POST_JSON j FROM posts p JOIN users u ON u.id=p.user_id " +
                "ORDER BY p.created_at DESC,p.id DESC LIMIT 20)"
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

    fun feed(): ByteArray = feed.executeQuery().use { it.next(); it.getBytes(1) }

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
