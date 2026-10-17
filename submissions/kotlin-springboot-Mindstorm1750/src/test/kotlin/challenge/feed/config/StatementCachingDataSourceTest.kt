package challenge.feed.config

import challenge.feed.TestDatabase
import org.sqlite.SQLiteDataSource
import java.sql.Connection
import java.sql.SQLException
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertSame
import kotlin.test.assertTrue

class StatementCachingDataSourceTest {
    private val dbPath = TestDatabase.create()
    private val dataSource = StatementCachingDataSource(SQLiteDataSource().apply { url = "jdbc:sqlite:$dbPath" })
    private val open = mutableListOf<Connection>()

    private fun connect(): Connection = dataSource.connection.also(open::add)

    private fun Connection.postCount(): Int =
        prepareStatement("SELECT count(*) FROM posts").use { ps -> ps.executeQuery().use { rs -> rs.next(); rs.getInt(1) } }

    @AfterTest
    fun closeConnections() = open.forEach(Connection::close)

    @Test
    fun `the same SQL on one connection reuses one compiled statement`() {
        val conn = connect()
        val first = conn.prepareStatement("SELECT id FROM posts WHERE id = ?")
        first.close()
        assertSame(first, conn.prepareStatement("SELECT id FROM posts WHERE id = ?"))
    }

    @Test
    fun `parameters do not leak into the next use`() {
        val conn = connect()
        conn.prepareStatement("SELECT ?").use { ps ->
            ps.setString(1, "secret")
            ps.executeQuery().use { rs -> rs.next(); assertEquals("secret", rs.getString(1)) }
        }
        conn.prepareStatement("SELECT ?").use { ps ->
            ps.executeQuery().use { rs -> rs.next(); assertNull(rs.getString(1)) }
        }
    }

    @Test
    fun `a reused statement sees rows committed by other connections`() {
        val reader = connect()
        val writer = connect()
        val before = reader.postCount()
        writer.prepareStatement("INSERT INTO posts (user_id, body) VALUES (1, 'new')").use { it.executeUpdate() }
        assertEquals(before + 1, reader.postCount())
    }

    @Test
    fun `a cached statement does not hold a read snapshot open between uses`() {
        val reader = connect()
        val writer = connect()
        reader.postCount()
        writer.prepareStatement("INSERT INTO posts (user_id, body) VALUES (1, 'new')").use { it.executeUpdate() }
        // A full checkpoint only succeeds when no connection is still reading an older snapshot.
        writer.createStatement().use { st ->
            st.executeQuery("PRAGMA wal_checkpoint(TRUNCATE)").use { rs -> rs.next(); assertEquals(0, rs.getInt(1), "checkpoint was blocked") }
        }
    }

    @Test
    fun `the cache is bounded, so dynamic SQL cannot grow it without limit`() {
        val conn = connect()
        val oldest = conn.prepareStatement("SELECT 0")
        oldest.close()
        repeat(StatementCachingDataSource.MAX_STATEMENTS_PER_CONNECTION + 10) { conn.prepareStatement("SELECT ${it + 1}").close() }
        assertTrue(oldest.isClosed, "least recently used statement was not evicted and closed")
    }

    @Test
    fun `a statement that is still in use is never evicted`() {
        val conn = connect()
        val inUse = conn.prepareStatement("SELECT 0")
        repeat(StatementCachingDataSource.MAX_STATEMENTS_PER_CONNECTION + 10) { conn.prepareStatement("SELECT ${it + 1}").close() }
        assertFalse(inUse.isClosed)
        inUse.executeQuery().use { rs -> rs.next(); assertEquals(0, rs.getInt(1)) }
        inUse.close()
    }

    @Test
    fun `a statement the driver closed after an error is prepared again`() {
        // sqlite-jdbc closes a statement whose step fails with most errors (I/O, disk full, runtime errors);
        // the cache must not keep handing out that dead statement.
        val conn = connect()
        fun abs(value: Long) = conn.prepareStatement("SELECT abs(?)").use { ps ->
            ps.setLong(1, value)
            ps.executeQuery().use { rs -> rs.next(); rs.getLong(1) }
        }
        assertEquals(5, abs(-5))
        assertFailsWith<SQLException> { abs(Long.MIN_VALUE) } // integer overflow
        assertEquals(7, abs(-7))
    }

    @Test
    fun `closing the connection really closes its statements`() {
        val conn = dataSource.connection
        val statement = conn.prepareStatement("SELECT 1")
        statement.close()
        conn.close()
        assertTrue(statement.isClosed)
    }
}
