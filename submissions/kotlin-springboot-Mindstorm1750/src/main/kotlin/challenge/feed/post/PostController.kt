package challenge.feed.post

import challenge.feed.auth.AuthUser
import org.springframework.http.HttpStatus
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.PathVariable
import org.springframework.web.bind.annotation.PostMapping
import org.springframework.web.bind.annotation.RequestBody
import org.springframework.web.bind.annotation.RestController

/**
 * Ids arrive as strings and are parsed in the method body, after every argument (including [AuthUser])
 * has resolved; that is what makes "auth before id" hold.
 */
@RestController
class PostController(private val posts: PostService) {

    @GetMapping("/feed")
    fun feed() = Feed(posts.feed())

    @GetMapping("/posts/{id}")
    fun get(@PathVariable id: String) = PostEnvelope(posts.get(PostId.parse(id)))

    @PostMapping("/posts")
    fun create(user: AuthUser, @RequestBody request: CreatePostRequest): ResponseEntity<PostEnvelope> =
        ResponseEntity.status(HttpStatus.CREATED).body(PostEnvelope(posts.create(user, request.body)))

    @PostMapping("/posts/{id}/like")
    fun like(user: AuthUser, @PathVariable id: String): ResponseEntity<LikeResult> {
        val result = posts.like(user, PostId.parse(id))
        return ResponseEntity.status(if (result.alreadyLiked) HttpStatus.OK else HttpStatus.CREATED).body(result)
    }
}
