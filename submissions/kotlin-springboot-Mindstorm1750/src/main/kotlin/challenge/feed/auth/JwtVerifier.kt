package challenge.feed.auth

import challenge.feed.error.Unauthorized
import challenge.feed.toPositiveLongOrNull
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.doubleOrNull
import java.security.MessageDigest
import java.time.Clock
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Verifies HS256 JWTs: signature (constant-time), algorithm (HS256 only) and expiry, then extracts the user.
 * Every token is verified on every request; nothing is cached.
 */
class JwtVerifier(secret: String, private val clock: Clock) {

    // Mac instances are not thread-safe; cloning an initialised one is cheaper than Mac.getInstance + init.
    private val prototype: Mac = Mac.getInstance(ALGORITHM).apply {
        init(SecretKeySpec(secret.toByteArray(Charsets.UTF_8), ALGORITHM))
    }

    fun verify(token: String): AuthUser {
        val parts = token.split('.')
        if (parts.size != 3) throw Unauthorized(INVALID_TOKEN)
        val (header, payload, signature) = parts

        if (decodeObject(header)?.string("alg") != "HS256") throw Unauthorized(INVALID_TOKEN)
        val expected = (prototype.clone() as Mac).doFinal("$header.$payload".toByteArray(Charsets.US_ASCII))
        if (!MessageDigest.isEqual(expected, decode(signature))) throw Unauthorized(INVALID_TOKEN)

        val claims = decodeObject(payload) ?: throw Unauthorized(INVALID_TOKEN)
        val exp = (claims["exp"] as? JsonPrimitive)?.takeUnless { it.isString }?.doubleOrNull
        if (exp == null || exp <= clock.instant().epochSecond) throw Unauthorized(INVALID_TOKEN)

        val id = claims.string("sub")?.toPositiveLongOrNull()
        val username = claims.string("username")
        if (id == null || username == null) throw Unauthorized(INVALID_PAYLOAD)
        return AuthUser(id, username)
    }

    private fun decode(part: String): ByteArray? =
        try {
            Base64.getUrlDecoder().decode(part)
        } catch (e: IllegalArgumentException) {
            null
        }

    private fun decodeObject(part: String): JsonObject? {
        val bytes = decode(part) ?: return null
        return try {
            Json.parseToJsonElement(bytes.decodeToString()) as? JsonObject
        } catch (e: IllegalArgumentException) { // SerializationException extends IllegalArgumentException
            null
        }
    }

    /** The claim's value if it is a JSON string, else null. */
    private fun JsonObject.string(name: String): String? =
        (this[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

    companion object {
        private const val ALGORITHM = "HmacSHA256"
        const val MISSING_TOKEN = "missing bearer token"
        const val INVALID_TOKEN = "invalid or expired token"
        const val INVALID_PAYLOAD = "invalid token payload"
    }
}
