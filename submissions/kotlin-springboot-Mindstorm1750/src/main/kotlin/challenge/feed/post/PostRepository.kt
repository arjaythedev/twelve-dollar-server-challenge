package challenge.feed.post

import org.springframework.beans.factory.annotation.Qualifier
import org.springframework.jdbc.core.RowMapper
import org.springframework.jdbc.core.simple.JdbcClient
import org.springframework.stereotype.Repository

@Repository
class PostRepository(
    @Qualifier("readerJdbc") private val reader: JdbcClient,
    @Qualifier("writerJdbc") private val writer: JdbcClient,
) {
    fun findById(id: Long): Post? =
        reader.sql("$SELECT_POSTS WHERE p.id = ?").param(id).query(POST).optional().orElse(null)

    fun latest(limit: Int): List<Post> =
        reader.sql("$SELECT_POSTS ORDER BY p.created_at DESC, p.id DESC LIMIT ?").param(limit).query(POST).list()

    fun insert(userId: Long, author: String, body: String): Post =
        writer.sql("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at")
            .params(userId, body)
            .query { rs, _ -> Post(rs.getLong(1), body, rs.getString(2), author, likeCount = 0) }
            .single()

    /** True if a like was inserted; false if it already existed or the post doesn't exist. */
    fun insertLike(userId: Long, postId: Long): Boolean =
        writer.sql(
            """
            INSERT INTO likes (user_id, post_id)
            SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?)
            ON CONFLICT (user_id, post_id) DO NOTHING
            """.trimIndent(),
        ).params(userId, postId, postId).update() == 1

    fun exists(postId: Long): Boolean =
        reader.sql("SELECT 1 FROM posts WHERE id = ?").param(postId).query(Int::class.javaObjectType).optional().isPresent

    private companion object {
        const val SELECT_POSTS = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id"""

        val POST = RowMapper { rs, _ ->
            Post(rs.getLong(1), rs.getString(2), rs.getString(3), rs.getString(4), rs.getLong(5))
        }
    }
}
