package challenge.feed

import java.time.Instant
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** Builds JWTs for tests, the same way test/test.sh does with openssl. */
object TestTokens {
    const val SECRET = "test-secret"
    const val HS256_HEADER = """{"alg":"HS256","typ":"JWT"}"""

    fun sign(payload: String, secret: String = SECRET, header: String = HS256_HEADER): String {
        val encoder = Base64.getUrlEncoder().withoutPadding()
        val signingInput = encoder.encodeToString(header.toByteArray()) + "." + encoder.encodeToString(payload.toByteArray())
        val mac = Mac.getInstance("HmacSHA256").apply { init(SecretKeySpec(secret.toByteArray(), "HmacSHA256")) }
        return signingInput + "." + encoder.encodeToString(mac.doFinal(signingInput.toByteArray()))
    }

    fun valid(sub: String = "1", username: String = "alice", secret: String = SECRET): String {
        val now = Instant.now().epochSecond
        return sign("""{"sub":"$sub","username":"$username","iat":$now,"exp":${now + 3600}}""", secret)
    }
}
