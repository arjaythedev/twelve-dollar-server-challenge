package dev.syoritohatsuki.tdsck.plugins

import com.auth0.jwt.JWT
import com.auth0.jwt.algorithms.Algorithm
import com.auth0.jwt.exceptions.JWTVerificationException
import dev.syoritohatsuki.tdsck.utils.JWT_SECRET
import dev.syoritohatsuki.tdsck.utils.positiveIntegerRegex
import io.ktor.http.*
import io.ktor.server.application.*
import io.ktor.server.auth.*
import io.ktor.server.auth.jwt.*
import io.ktor.server.response.*

fun Application.configureAuthentication() {
    install(Authentication) {
        val verifier = JWT.require(Algorithm.HMAC256(JWT_SECRET)).build()

        jwt {
            verifier(verifier)

            validate { credential ->
                val payload = credential.payload
                val subject = payload.subject ?: return@validate null

                if (!positiveIntegerRegex.matches(subject)) return@validate null
                subject.toLongOrNull() ?: return@validate null

                payload.getClaim("username").asString() ?: return@validate null

                JWTPrincipal(payload)
            }

            challenge { _, _ ->
                val authorization = call.request.headers[HttpHeaders.Authorization]

                if (authorization == null || !authorization.startsWith("Bearer ")) {
                    return@challenge call.respond(
                        status = HttpStatusCode.Unauthorized, message = mapOf("error" to "missing bearer token")
                    )
                }

                val message = try {
                    val decoded = verifier.verify(authorization.removePrefix("Bearer "))

                    val subject = decoded.subject
                    val username = decoded.getClaim("username").asString()

                    when {
                        subject == null || !positiveIntegerRegex.matches(subject) || subject.toLongOrNull() == null || username == null -> "invalid token payload"
                        else -> "invalid or expired token"
                    }
                } catch (_: JWTVerificationException) {
                    "invalid or expired token"
                } catch (_: IllegalArgumentException) {
                    "invalid or expired token"
                }

                call.respond(status = HttpStatusCode.Unauthorized, message = mapOf("error" to message))
            }
        }
    }
}