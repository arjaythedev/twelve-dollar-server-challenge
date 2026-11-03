package challenge.feed

import java.nio.file.Files
import java.nio.file.Path
import java.sql.Connection
import java.sql.DriverManager

/** A small WAL-mode database with the challenge schema and a fixed set of rows, for integration tests. */
object TestDatabase {
    const val POST_COUNT = 25

    fun create(): Path {
        val path = Files.createTempDirectory("feed-test").resolve("feed.db")
        connect(path).use { conn ->
            conn.createStatement().use { it.execute("PRAGMA journal_mode=WAL") }
            schemaStatements().forEach { sql -> conn.createStatement().use { it.execute(sql) } }
        }
        reset(path)
        return path
    }

    fun reset(path: Path) {
        connect(path).use { conn ->
            conn.autoCommit = false
            conn.createStatement().use { st ->
                st.execute("DELETE FROM likes")
                st.execute("DELETE FROM posts")
                st.execute("DELETE FROM users")
                st.execute("INSERT INTO users (id, username) VALUES (1, 'alice'), (2, 'bob')")
            }
            conn.prepareStatement("INSERT INTO posts (id, user_id, body, created_at) VALUES (?, ?, ?, ?)").use { ps ->
                for (id in 1..POST_COUNT) {
                    ps.setInt(1, id)
                    ps.setInt(2, if (id % 2 == 1) 1 else 2)
                    ps.setString(3, "post $id")
                    ps.setString(4, createdAt(id))
                    ps.addBatch()
                }
                ps.executeBatch()
            }
            conn.createStatement().use { it.execute("INSERT INTO likes (user_id, post_id) VALUES (1, 25), (2, 25), (2, 1)") }
            conn.commit()
        }
    }

    fun createdAt(id: Int) = "2025-01-01T00:00:%02d.000Z".format(id)

    fun author(id: Int) = if (id % 2 == 1) "alice" else "bob"

    fun likeCount(id: Int) = when (id) { 25 -> 2; 1 -> 1; else -> 0 }

    /** The exact JSON the API must produce for fixture post [id]. */
    fun postJson(id: Int) =
        """{"id":$id,"body":"post $id","created_at":"${createdAt(id)}","author":"${author(id)}","like_count":${likeCount(id)}}"""

    private fun connect(path: Path): Connection = DriverManager.getConnection("jdbc:sqlite:$path")

    private fun schemaStatements(): List<String> =
        javaClass.getResource("/schema.sql")!!.readText()
            .lines().filterNot { it.trimStart().startsWith("--") }.joinToString("\n")
            .split(';').map { it.trim() }.filter { it.isNotEmpty() }
}
