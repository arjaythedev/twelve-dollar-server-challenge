package challenge.feed.auth

import challenge.feed.TestTokens
import challenge.feed.error.Unauthorized
import java.time.Clock
import java.time.Instant
import java.time.ZoneOffset
import java.util.Base64
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class JwtVerifierTest {
    private val now = Instant.parse("2026-01-01T00:00:00Z")
    private val verifier = JwtVerifier(TestTokens.SECRET, Clock.fixed(now, ZoneOffset.UTC))
    private val exp = now.epochSecond + 3600

    private fun payload(sub: String = "\"7\"", username: String = "\"alice\"", exp: String = "${this.exp}") =
        """{"sub":$sub,"username":$username,"iat":${now.epochSecond},"exp":$exp}"""

    private fun rejected(token: String): String = assertFailsWith<Unauthorized> { verifier.verify(token) }.message

    @Test
    fun `valid token yields the user`() {
        assertEquals(AuthUser(7, "alice"), verifier.verify(TestTokens.sign(payload())))
    }

    @Test
    fun `username is used as-is`() {
        assertEquals(AuthUser(7, "golden ember"), verifier.verify(TestTokens.sign(payload(username = "\"golden ember\""))))
    }

    @Test
    fun `malformed tokens are invalid`() {
        for (token in listOf("abc", "a.b", "a.b.c.d", "", "...", "!!!.@@@.###")) {
            assertEquals(JwtVerifier.INVALID_TOKEN, rejected(token), "token '$token'")
        }
    }

    @Test
    fun `bad signature is invalid`() {
        val token = TestTokens.sign(payload(), secret = "another-secret")
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(token))
    }

    @Test
    fun `tampered payload is invalid`() {
        val (h, _, s) = TestTokens.sign(payload()).split('.')
        val forged = Base64.getUrlEncoder().withoutPadding().encodeToString(payload(sub = "\"1\"").toByteArray())
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected("$h.$forged.$s"))
    }

    @Test
    fun `alg none is invalid`() {
        val unsigned = TestTokens.sign(payload(), header = """{"alg":"none","typ":"JWT"}""").substringBeforeLast('.') + "."
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(unsigned))
    }

    @Test
    fun `other algorithms are invalid even when the HMAC matches`() {
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(TestTokens.sign(payload(), header = """{"alg":"HS512","typ":"JWT"}""")))
    }

    @Test
    fun `expired token is invalid`() {
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(TestTokens.sign(payload(exp = "${now.epochSecond - 1}"))))
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(TestTokens.sign(payload(exp = "${now.epochSecond}"))))
    }

    @Test
    fun `token without exp is rejected`() {
        val token = TestTokens.sign("""{"sub":"7","username":"alice"}""")
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(token))
    }

    @Test
    fun `token with string exp is rejected`() {
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(TestTokens.sign(payload(exp = "\"${exp}\""))))
    }

    @Test
    fun `sub must be a positive integer string`() {
        for (sub in listOf("\"abc\"", "\"0\"", "\"-1\"", "7", "null", "\"\"")) {
            assertEquals(JwtVerifier.INVALID_PAYLOAD, rejected(TestTokens.sign(payload(sub = sub))), "sub $sub")
        }
    }

    @Test
    fun `username must be a string`() {
        assertEquals(JwtVerifier.INVALID_PAYLOAD, rejected(TestTokens.sign("""{"sub":"7","exp":$exp}""")))
        assertEquals(JwtVerifier.INVALID_PAYLOAD, rejected(TestTokens.sign(payload(username = "42"))))
    }

    @Test
    fun `payload that is not a JSON object is invalid`() {
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(TestTokens.sign("[1,2,3]")))
        assertEquals(JwtVerifier.INVALID_TOKEN, rejected(TestTokens.sign("not json")))
    }
}
