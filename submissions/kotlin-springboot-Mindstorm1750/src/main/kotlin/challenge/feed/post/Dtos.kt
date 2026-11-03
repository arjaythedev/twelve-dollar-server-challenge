package challenge.feed.post

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement

// Property order is the JSON key order the spec requires.

@Serializable
data class Post(
    val id: Long,
    val body: String,
    @SerialName("created_at") val createdAt: String,
    val author: String,
    @SerialName("like_count") val likeCount: Long,
)

@Serializable
data class PostEnvelope(val post: Post)

@Serializable
data class Feed(val posts: List<Post>)

@Serializable
data class LikeResult(
    val liked: Boolean,
    @SerialName("already_liked") val alreadyLiked: Boolean,
    @SerialName("post_id") val postId: Long,
)

/** `body` stays untyped so a number or `null` becomes 400 `body is required`, not a parse error. */
@Serializable
data class CreatePostRequest(val body: JsonElement? = null)
