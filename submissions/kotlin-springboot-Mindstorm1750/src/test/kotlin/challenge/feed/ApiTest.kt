package challenge.feed

import challenge.feed.health.HealthStatus
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.jupiter.api.BeforeEach
import org.springframework.beans.factory.annotation.Autowired
import org.springframework.boot.test.context.SpringBootTest
import org.springframework.boot.test.web.server.LocalServerPort
import org.springframework.test.context.DynamicPropertyRegistry
import org.springframework.test.context.DynamicPropertySource
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Instant
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/** Exact-bytes checks over real HTTP, mirroring test/test.sh against a small fixture database. */
@SpringBootTest(
    webEnvironment = SpringBootTest.WebEnvironment.RANDOM_PORT,
    properties = ["feed.jwt-secret=${TestTokens.SECRET}", "server.address=127.0.0.1"],
)
class ApiTest {
    @LocalServerPort var port = 0
    @Autowired lateinit var json: Json

    private val http = HttpClient.newHttpClient()
    private val alice = "Bearer ${TestTokens.valid(sub = "1", username = "alice")}"

    data class Reply(val status: Int, val body: String, val contentType: String)

    private fun call(method: String, path: String, auth: String? = null, body: String? = null): Reply {
        val request = HttpRequest.newBuilder(URI.create("http://127.0.0.1:$port$path")).apply {
            if (auth != null) header("Authorization", auth)
            if (body != null) header("Content-Type", "application/json")
            method(method, if (body != null) HttpRequest.BodyPublishers.ofString(body) else HttpRequest.BodyPublishers.noBody())
        }.build()
        val response = http.send(request, HttpResponse.BodyHandlers.ofString())
        return Reply(response.statusCode(), response.body(), response.headers().firstValue("Content-Type").orElse(""))
    }

    private fun assertReply(status: Int, body: String, reply: Reply) {
        assertEquals(status to body, reply.status to reply.body)
        assertTrue(reply.contentType.startsWith("application/json"), "content type '${reply.contentType}'")
    }

    private fun error(message: String) = """{"error":"$message"}"""

    @BeforeEach
    fun resetDatabase() = TestDatabase.reset(dbPath)

    @Test
    fun `feed is exact`() {
        val posts = (25 downTo 6).joinToString(",") { TestDatabase.postJson(it) }
        assertReply(200, """{"posts":[$posts]}""", call("GET", "/feed"))
    }

    @Test
    fun `single post is exact`() {
        assertReply(200, """{"post":${TestDatabase.postJson(1)}}""", call("GET", "/posts/1"))
        assertReply(200, """{"post":${TestDatabase.postJson(25)}}""", call("GET", "/posts/25"))
    }

    @Test
    fun `health reports ok with uptime`() {
        val reply = call("GET", "/health")
        assertEquals(200, reply.status)
        assertTrue(Regex("""\{"status":"ok","db":"ok","uptime_s":\d+}""").matches(reply.body), reply.body)
    }

    @Test
    fun `degraded health body has the spec's shape`() {
        assertEquals(
            """{"status":"degraded","db":"unreachable","error":"disk I/O error"}""",
            json.encodeToString(HealthStatus.serializer(), HealthStatus.down("disk I/O error")),
        )
    }

    @Test
    fun `unknown paths are 404`() {
        assertReply(404, error("not found"), call("GET", "/nope"))
        assertReply(404, error("not found"), call("GET", "/feed/extra"))
        assertReply(404, error("not found"), call("GET", "/error"))
    }

    @Test
    fun `invalid and missing post ids`() {
        for (id in listOf("0", "-1", "abc", "1.5")) assertReply(400, error("invalid post id"), call("GET", "/posts/$id"))
        assertReply(404, error("post not found"), call("GET", "/posts/999999999"))
    }

    @Test
    fun `auth errors`() {
        val body = """{"body":"x"}"""
        assertReply(401, error("missing bearer token"), call("POST", "/posts", null, body))
        assertReply(401, error("missing bearer token"), call("POST", "/posts", "Basic abc", body))
        assertReply(401, error("invalid or expired token"), call("POST", "/posts", "Bearer abc", body))
        assertReply(401, error("invalid or expired token"), call("POST", "/posts", "Bearer ${TestTokens.valid(secret = "wrong")}", body))
        val now = Instant.now().epochSecond
        val expired = TestTokens.sign("""{"sub":"1","username":"alice","exp":${now - 10}}""")
        assertReply(401, error("invalid or expired token"), call("POST", "/posts", "Bearer $expired", body))
        assertReply(401, error("invalid token payload"), call("POST", "/posts", "Bearer ${TestTokens.valid(sub = "abc")}", body))
    }

    @Test
    fun `auth is checked before the post id`() {
        assertReply(401, error("missing bearer token"), call("POST", "/posts/abc/like"))
        assertReply(400, error("invalid post id"), call("POST", "/posts/abc/like", alice))
    }

    @Test
    fun `auth is checked before the request body is parsed`() {
        assertReply(401, error("missing bearer token"), call("POST", "/posts", null, "{bad json"))
    }

    @Test
    fun `create validation`() {
        for (body in listOf("""{"body":""}""", """{"body":"   "}""", """{"body":123}""", "{}", """{"body":null}""")) {
            assertReply(400, error("body is required"), call("POST", "/posts", alice, body))
        }
        assertReply(400, error("malformed JSON body"), call("POST", "/posts", alice, "{bad json"))
        assertReply(400, error("body must be at most 500 characters"), call("POST", "/posts", alice, """{"body":"${"a".repeat(501)}"}"""))
    }

    @Test
    fun `create returns the trimmed post and it tops the feed`() {
        val reply = call("POST", "/posts", alice, """{"body":"  ${"b".repeat(500)} ","extra":1}""")
        assertEquals(201, reply.status, reply.body)
        val match = assertNotNull(
            Regex("""\{"post":\{"id":(\d+),"body":"b{500}","created_at":"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z)","author":"alice","like_count":0}}""")
                .matchEntire(reply.body),
            reply.body,
        )
        val (id, createdAt) = match.destructured
        val feed = call("GET", "/feed").body
        assertTrue(feed.startsWith("""{"posts":[{"id":$id,"body":"${"b".repeat(500)}","created_at":"$createdAt","author":"alice","like_count":0}"""), feed)
    }

    @Test
    fun `special characters round-trip`() {
        val reply = call("POST", "/posts", alice, """{"body":"  test: <b>&amp;</b> \"quoted\" / café ✓  "}""")
        assertEquals(201, reply.status)
        val body = Json.parseToJsonElement(reply.body).jsonObject["post"]!!.jsonObject["body"]!!.jsonPrimitive.content
        assertEquals("""test: <b>&amp;</b> "quoted" / café ✓""", body)
    }

    @Test
    fun `500 non-ascii characters round-trip`() {
        val reply = call("POST", "/posts", alice, """{"body":"${"é".repeat(500)}"}""")
        assertEquals(201, reply.status, reply.body)
        assertTrue(reply.body.contains(""""body":"${"é".repeat(500)}""""), reply.body)
    }

    @Test
    fun `like, repeat like, read back`() {
        assertReply(201, """{"liked":true,"already_liked":false,"post_id":3}""", call("POST", "/posts/3/like", alice))
        assertReply(200, """{"liked":true,"already_liked":true,"post_id":3}""", call("POST", "/posts/3/like", alice))
        assertReply(404, error("post not found"), call("POST", "/posts/999999999/like", alice))
        assertTrue(call("GET", "/posts/3").body.endsWith(""""like_count":1}}"""))
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
