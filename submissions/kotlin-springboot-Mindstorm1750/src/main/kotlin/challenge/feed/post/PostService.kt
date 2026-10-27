package challenge.feed.post

import challenge.feed.auth.AuthUser
import challenge.feed.error.NotFound
import kotlinx.serialization.json.JsonElement
import org.springframework.stereotype.Service

@Service
class PostService(private val posts: PostRepository) {

    fun feed(): List<Post> = posts.latest(FEED_SIZE)

    fun get(id: Long): Post = posts.findById(id) ?: throw NotFound("post not found")

    fun create(author: AuthUser, rawBody: JsonElement?): Post =
        posts.insert(author.id, author.username, PostBody.parse(rawBody))

    fun like(user: AuthUser, postId: Long): LikeResult = when {
        posts.insertLike(user.id, postId) -> LikeResult(liked = true, alreadyLiked = false, postId = postId)
        posts.exists(postId) -> LikeResult(liked = true, alreadyLiked = true, postId = postId)
        else -> throw NotFound("post not found")
    }

    companion object {
        const val FEED_SIZE = 20
    }
}
