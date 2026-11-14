@file:Suppress("SqlSourceToSinkFlow")

package dev.noamm9.velox.plugins

import dev.noamm9.velox.utils.SQLITE_PATH
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.withContext
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import java.sql.Connection
import java.sql.DriverManager
import java.sql.PreparedStatement
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
    val liked: Boolean,
    @SerialName("already_liked") val alreadyLiked: Boolean,
    @SerialName("post_id") val postId: Int
)

private const val POOL_SIZE = 10

// sqlite-jdbc only runs the first statement of a multi-statement string, so each pragma is executed on its own
private val PRAGMAS = listOf(
    "PRAGMA journal_mode = WAL",
    "PRAGMA synchronous = NORMAL",
    "PRAGMA busy_timeout = 5000",
    "PRAGMA mmap_size = 1073741824",
    "PRAGMA cache_size = -65536",
    "PRAGMA temp_store = MEMORY"
)

private class SqliteConnection(path: String) {
    private val connection: Connection = DriverManager.getConnection("jdbc:sqlite:$path").apply {
        createStatement().use { statement -> PRAGMAS.forEach(statement::execute) }
    }

    val health: PreparedStatement = connection.prepareStatement("SELECT 1")

    val feed: PreparedStatement = connection.prepareStatement(
        """
            SELECT p.id, p.body, p.created_at, u.username,
                (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
            FROM posts p JOIN users u ON u.id = p.user_id
            ORDER BY p.created_at DESC, p.id DESC LIMIT 20
        """.trimIndent()
    )

    val post: PreparedStatement = connection.prepareStatement(
        """
            SELECT p.id, p.body, p.created_at, u.username,
                (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
            FROM posts p JOIN users u ON u.id = p.user_id
            WHERE p.id = ?
        """.trimIndent()
    )

    val insertPost: PreparedStatement = connection.prepareStatement(
        "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at"
    )

    val insertLike: PreparedStatement = connection.prepareStatement(
        """
            INSERT INTO likes (user_id, post_id)
            SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
            ON CONFLICT (user_id, post_id) DO NOTHING
        """.trimIndent()
    )

    val postExists: PreparedStatement = connection.prepareStatement("SELECT 1 FROM posts WHERE id = ?")
}

private val connections = Channel<SqliteConnection>(POOL_SIZE)

fun configureDatabase() {
    repeat(POOL_SIZE) {
        connections.trySend(SqliteConnection(SQLITE_PATH))
    }
}

private suspend fun <T> withConnection(block: SqliteConnection.() -> T): Result<T> {
    val connection = connections.receive()

    return try {
        withContext(Dispatchers.IO) {
            runCatching { connection.block() }
        }
    }
    finally {
        connections.trySend(connection)
    }
}

suspend fun health(): Result<Boolean> = withConnection {
    health.executeQuery().use(ResultSet::next)
}

suspend fun getFeed(): Result<List<Post>> = withConnection {
    feed.executeQuery().use { resultSet ->
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

suspend fun getPost(id: Int): Result<Post?> = withConnection {
    post.setInt(1, id)
    post.executeQuery().use { resultSet ->
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

suspend fun insertPost(userId: Int, username: String, body: String): Result<CreatedPost> = withConnection {
    insertPost.setInt(1, userId)
    insertPost.setString(2, body)

    insertPost.executeQuery().use { resultSet ->
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

suspend fun insertLike(userId: Int, postId: Int): Result<LikeResult?> = withConnection {
    insertLike.setInt(1, userId)
    insertLike.setInt(2, postId)

    if (insertLike.executeUpdate() == 1) {
        return@withConnection LikeResult(liked = true, alreadyLiked = false, postId = postId)
    }

    postExists.setInt(1, postId)
    postExists.executeQuery().use { resultSet ->
        when {
            resultSet.next() -> LikeResult(liked = true, alreadyLiked = true, postId = postId)
            else -> null
        }
    }
}
