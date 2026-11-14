package dev.noamm9.velox.plugins

import dev.noamm9.velox.utils.JWT_SECRET
import io.ktor.http.HttpHeaders
import io.ktor.http.HttpStatusCode
import io.ktor.server.application.ApplicationCall
import io.ktor.server.response.respond
import kotlinx.serialization.json.*
import java.security.MessageDigest
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

sealed interface AuthResult {
    data class Authenticated(val userId: Int, val username: String) : AuthResult
    data class Rejected(val error: String) : AuthResult
}

class JwtVerifier(secret: String) {
    private val key = SecretKeySpec(secret.toByteArray(), "HmacSHA256")
    private val mac = ThreadLocal.withInitial { Mac.getInstance("HmacSHA256").apply { init(key) } }
    private val decoder = Base64.getUrlDecoder()

    fun verify(authorization: String?): AuthResult {
        if (authorization == null || ! authorization.startsWith("Bearer ")) return AuthResult.Rejected("missing bearer token")

        val payload = runCatching { verifiedPayload(authorization.removePrefix("Bearer ")) }.getOrNull()
            ?: return AuthResult.Rejected("invalid or expired token")

        val userId = payload.string("sub")?.toIntOrNull()?.takeIf { it > 0 } ?: return AuthResult.Rejected("invalid token payload")
        val username = payload.string("username") ?: return AuthResult.Rejected("invalid token payload")

        return AuthResult.Authenticated(userId, username)
    }

    private fun verifiedPayload(token: String): JsonObject? {
        val parts = token.split('.')
        if (parts.size != 3) return null

        val header = Json.parseToJsonElement(decoder.decode(parts[0]).decodeToString()).jsonObject
        if (header.string("alg") != "HS256") return null

        val signature = mac.get().doFinal("${parts[0]}.${parts[1]}".toByteArray())
        if (! MessageDigest.isEqual(signature, decoder.decode(parts[2]))) return null

        val payload = Json.parseToJsonElement(decoder.decode(parts[1]).decodeToString()).jsonObject
        val now = System.currentTimeMillis() / 1000

        // same time checks java-jwt did: exp in the past, nbf or iat in the future
        if (payload["exp"]?.jsonPrimitive?.long?.let { now > it } == true) return null
        if (payload["nbf"]?.jsonPrimitive?.long?.let { now < it } == true) return null
        if (payload["iat"]?.jsonPrimitive?.long?.let { now < it } == true) return null

        return payload
    }

    private fun JsonObject.string(name: String) = (get(name) as? JsonPrimitive)?.takeIf { it.isString }?.content
}

private val verifier = JwtVerifier(JWT_SECRET)

suspend fun ApplicationCall.authenticate(): AuthResult.Authenticated? =
    when (val result = verifier.verify(request.headers[HttpHeaders.Authorization])) {
        is AuthResult.Authenticated -> result
        is AuthResult.Rejected -> {
            respond(status = HttpStatusCode.Unauthorized, message = mapOf("error" to result.error))
            null
        }
    }
