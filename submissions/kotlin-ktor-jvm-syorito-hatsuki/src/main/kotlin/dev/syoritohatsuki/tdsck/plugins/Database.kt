@file:Suppress("SqlSourceToSinkFlow", "LoggingSimilarMessage")

package dev.syoritohatsuki.tdsck.plugins

import com.zaxxer.hikari.HikariConfig
import com.zaxxer.hikari.HikariDataSource
import dev.syoritohatsuki.tdsck.utils.SQLITE_PATH
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import java.sql.ResultSet


@Serializable
data class Post(
    val id: Int,
    val body: String,
    @SerialName("created_at") val createdAt: String,
    val author: String,
    @SerialName("like_count") val likeCount: Int
)

@Serializable
data class CreatedPost(
    val id: Int,
    val body: String,
    @SerialName("created_at") val createdAt: String,
    val author: String,
    @SerialName("like_count") val likeCount: Int
)

@Serializable
data class LikeResult(
    val liked: Boolean, @SerialName("already_liked") val alreadyLiked: Boolean, @SerialName("post_id") val postId: Int
)

lateinit var sqliteDataSource: HikariDataSource

fun configureDatabase() {
    sqliteDataSource = HikariDataSource(HikariConfig().apply {
        jdbcUrl = "jdbc:sqlite:$SQLITE_PATH"

        connectionInitSql = """
            PRAGMA journal_mode = WAL;
            PRAGMA synchronous = NORMAL;
            PRAGMA busy_timeout = 5000;
            PRAGMA mmap_size = 1073741824;
            PRAGMA cache_size = -65536;
            PRAGMA temp_store = MEMORY;
        """.trimIndent()

        validate()
    })
}

/*   Utils   */

fun ResultSet.dump(): String {
    val meta = metaData
    val columns = meta.columnCount

    return buildString {
        while (next()) {
            for (i in 1..columns) {
                if (i > 1) append(", ")
                append(meta.getColumnLabel(i))
                append("=")
                append(getObject(i))
            }
            appendLine()
        }
    }
}

/*   Data   */

suspend fun health(): Result<Boolean> = withContext(Dispatchers.IO) {
    runCatching {
        sqliteDataSource.connection.use { connection ->
            connection.prepareStatement("SELECT 1").use { statement ->
                statement.executeQuery().use(ResultSet::next)
            }
        }
    }
}

suspend fun getFeed(): Result<List<Post>> = withContext(Dispatchers.IO) {
    runCatching {
        sqliteDataSource.connection.use { connection ->
            connection.prepareStatement(
                """
                    SELECT p.id, p.body, p.created_at, u.username,
                        (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
                    FROM posts p JOIN users u ON u.id = p.user_id
                    ORDER BY p.created_at DESC, p.id DESC LIMIT 20
                """.trimIndent()
            ).use { statement ->
                statement.executeQuery().use { resultSet ->
                    buildList {
                        while (resultSet.next()) {
                            add(
                                Post(
                                    id = resultSet.getInt(1),
                                    body = resultSet.getString(2),
                                    createdAt = resultSet.getString(3),
                                    author = resultSet.getString(4),
                                    likeCount = resultSet.getInt(5)
                                )
                            )
                        }
                    }
                }
            }
        }
    }
}

suspend fun getPost(id: Int): Result<Post?> = withContext(Dispatchers.IO) {
    runCatching {
        sqliteDataSource.connection.use { connection ->
            connection.prepareStatement(
                """
                    SELECT p.id, p.body, p.created_at, u.username,
                        (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
                    FROM posts p JOIN users u ON u.id = p.user_id
                    WHERE p.id = ?
                """.trimIndent()
            ).use { statement ->
                statement.setInt(1, id)
                statement.executeQuery().use { resultSet ->
                    if (resultSet.next()) {
                        return@use Post(
                            id = resultSet.getInt(1),
                            body = resultSet.getString(2),
                            createdAt = resultSet.getString(3),
                            author = resultSet.getString(4),
                            likeCount = resultSet.getInt(5)
                        )
                    }
                    null
                }
            }
        }
    }
}

suspend fun insertPost(userId: Int, username: String, body: String): Result<CreatedPost> = withContext(Dispatchers.IO) {
    runCatching {
        sqliteDataSource.connection.use { connection ->
            connection.prepareStatement(
                "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
            ).use { statement ->
                statement.setInt(1, userId)
                statement.setString(2, body)

                statement.executeQuery().use { resultSet ->
                    resultSet.next()

                    CreatedPost(
                        id = resultSet.getInt(1),
                        body = body,
                        createdAt = resultSet.getString(2),
                        author = username,
                        likeCount = 0
                    )
                }
            }
        }
    }
}

suspend fun insertLike(userId: Int, postId: Int): Result<LikeResult?> = withContext(Dispatchers.IO) {
    runCatching {
        sqliteDataSource.connection.use { connection ->
            connection.prepareStatement(
                """
                    INSERT INTO likes (user_id, post_id)
                    SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
                    ON CONFLICT (user_id, post_id) DO NOTHING
                """.trimIndent()
            ).use { statement ->
                statement.setInt(1, userId)
                statement.setInt(2, postId)

                if (statement.executeUpdate() == 1) {
                    return@runCatching LikeResult(liked = true, alreadyLiked = false, postId = postId)
                }
            }

            connection.prepareStatement("SELECT 1 FROM posts WHERE id = ?").use { statement ->
                statement.setInt(1, postId)

                statement.executeQuery().use { resultSet ->
                    when {
                        resultSet.next() -> LikeResult(liked = true, alreadyLiked = true, postId = postId)
                        else -> null
                    }
                }
            }
        }
    }
}