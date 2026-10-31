package challenge.feed.post

import challenge.feed.TestDatabase
import challenge.feed.TestTokens
import challenge.feed.auth.AuthUser
import challenge.feed.error.BadRequest
import challenge.feed.error.NotFound
import kotlinx.serialization.json.JsonPrimitive
import org.junit.jupiter.api.BeforeEach
import org.springframework.beans.factory.annotation.Autowired
import org.springframework.boot.test.context.SpringBootTest
import org.springframework.test.context.DynamicPropertyRegistry
import org.springframework.test.context.DynamicPropertySource
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

@SpringBootTest(webEnvironment = SpringBootTest.WebEnvironment.NONE, properties = ["feed.jwt-secret=${TestTokens.SECRET}"])
class PostServiceTest {
    @Autowired lateinit var service: PostService

    private val alice = AuthUser(1, "alice")

    @BeforeEach
    fun resetDatabase() = TestDatabase.reset(dbPath)

    @Test
    fun `feed is the 20 newest posts with authors and like counts`() {
        val feed = service.feed()
        assertEquals((25 downTo 6).map(Int::toLong), feed.map { it.id })
        assertEquals(Post(25, "post 25", TestDatabase.createdAt(25), "alice", 2), feed.first())
        assertEquals(Post(6, "post 6", TestDatabase.createdAt(6), "bob", 0), feed.last())
    }

    @Test
    fun `get returns one post or throws not found`() {
        assertEquals(Post(1, "post 1", TestDatabase.createdAt(1), "alice", 1), service.get(1))
        assertEquals("post not found", assertFailsWith<NotFound> { service.get(999_999_999) }.message)
    }

    @Test
    fun `create stores a trimmed post and returns it with the token's username`() {
        val created = service.create(AuthUser(2, "name-from-token"), JsonPrimitive("  hello  "))
        assertEquals("hello", created.body)
        assertEquals("name-from-token", created.author)
        assertEquals(0, created.likeCount)
        assertTrue(Regex("""\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z""").matches(created.createdAt), created.createdAt)
        assertEquals(created.id, service.feed().first().id)
    }

    @Test
    fun `create validates the body`() {
        assertFailsWith<BadRequest> { service.create(alice, JsonPrimitive("   ")) }
    }

    @Test
    fun `like is created once and then reported as already liked`() {
        assertEquals(LikeResult(liked = true, alreadyLiked = false, postId = 3), service.like(alice, 3))
        assertEquals(LikeResult(liked = true, alreadyLiked = true, postId = 3), service.like(alice, 3))
        assertEquals(1, service.get(3).likeCount)
    }

    @Test
    fun `liking a missing post is not found`() {
        assertEquals("post not found", assertFailsWith<NotFound> { service.like(alice, 999_999_999) }.message)
    }

    @Test
    fun `concurrent likes from many users all succeed`() {
        val pool = Executors.newFixedThreadPool(32)
        try {
            val results = pool.invokeAll((1L..200L).map { userId -> Callable { service.like(AuthUser(userId, "u$userId"), 10) } })
                .map { it.get() }
            assertTrue(results.all { it.liked && !it.alreadyLiked })
            assertEquals(200, service.get(10).likeCount)
        } finally {
            pool.shutdown()
        }
    }

    companion object {
        private val dbPath = TestDatabase.create()

        @JvmStatic
        @DynamicPropertySource
        fun properties(registry: DynamicPropertyRegistry) {
            registry.add("feed.sqlite-path") { dbPath.toString() }
        }
    }
}
