package dev.syoritohatsuki.tdsck.routing

import dev.syoritohatsuki.tdsck.plugins.getPost
import dev.syoritohatsuki.tdsck.plugins.insertLike
import dev.syoritohatsuki.tdsck.plugins.insertPost
import dev.syoritohatsuki.tdsck.utils.positiveIntegerRegex
import io.ktor.http.*
import io.ktor.server.auth.*
import io.ktor.server.auth.jwt.*
import io.ktor.server.request.*
import io.ktor.server.response.*
import io.ktor.server.routing.*
import kotlinx.serialization.Serializable
import kotlinx.serialization.SerializationException
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonPrimitive

private const val JS_WHITESPACES =
    "\t\n\u000B\u000C\r \u00A0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029\u202F\u205F\u3000\uFEFF"

private fun parsePostId(raw: String?): Int? {
    if (raw == null || !positiveIntegerRegex.matches(raw)) return null
    return raw.toIntOrNull()?.takeIf { it in 1..Int.MAX_VALUE }
}

private fun String.trimJsWhitespace(): String = trim { it in JS_WHITESPACES }

@Serializable
data class CreatePostRequest(val body: String? = null)

fun Route.postsRoute() {
    route("posts") {
        get("{id}") {
            val id = parsePostId(call.pathParameters["id"]) ?: return@get call.respond(
                status = HttpStatusCode.BadRequest, message = mapOf("error" to "invalid post id")
            )

            getPost(id).onSuccess { post ->
                when (post) {
                    null -> call.respond(
                        status = HttpStatusCode.NotFound, message = mapOf("error" to "post not found")
                    )

                    else -> call.respond(mapOf("post" to post))
                }
            }.onFailure {
                call.respond(
                    status = HttpStatusCode.InternalServerError,
                    message = mapOf("error" to (it.message ?: "internal server error"))
                )
            }
        }

        authenticate {
            post {
                val principal = call.principal<JWTPrincipal>() ?: return@post call.respond(
                    status = HttpStatusCode.Unauthorized, message = mapOf("error" to "invalid token payload")
                )

                val userId = principal.payload.subject.toIntOrNull() ?: return@post call.respond(
                    status = HttpStatusCode.Unauthorized, message = mapOf("error" to "invalid token payload")
                )

                val username = principal.payload.getClaim("username").asString() ?: return@post call.respond(
                    status = HttpStatusCode.Unauthorized, message = mapOf("error" to "invalid token payload")
                )

                val json = try {
                    Json.parseToJsonElement(call.receiveText())
                } catch (_: SerializationException) {
                    return@post call.respond(
                        status = HttpStatusCode.BadRequest, message = mapOf("error" to "malformed JSON body")
                    )
                } catch (_: IllegalArgumentException) {
                    return@post call.respond(
                        status = HttpStatusCode.BadRequest, message = mapOf("error" to "malformed JSON body")
                    )
                }

                val body =
                    ((json as? JsonObject)?.get("body") as? JsonPrimitive)?.takeIf { it.isString }?.jsonPrimitive?.content?.trimJsWhitespace()
                        ?: return@post call.respond(
                            status = HttpStatusCode.BadRequest, message = mapOf("error" to "body is required")
                        )

                if (body.isEmpty()) {
                    return@post call.respond(
                        status = HttpStatusCode.BadRequest, message = mapOf("error" to "body is required")
                    )
                }

                if (body.length > 500) {
                    return@post call.respond(
                        status = HttpStatusCode.BadRequest,
                        message = mapOf("error" to "body must be at most 500 characters")
                    )
                }

                insertPost(userId, username, body).onSuccess { post ->
                    call.respond(
                        status = HttpStatusCode.Created, message = mapOf("post" to post)
                    )
                }.onFailure {
                    call.respond(
                        status = HttpStatusCode.InternalServerError, message = mapOf("error" to "internal server error")
                    )
                }
            }

            post("{id}/like") {
                val principal = call.principal<JWTPrincipal>() ?: return@post call.respond(
                    status = HttpStatusCode.Unauthorized, message = mapOf("error" to "invalid token payload")
                )

                val userId = principal.payload.subject.toIntOrNull() ?: return@post call.respond(
                    status = HttpStatusCode.Unauthorized, message = mapOf("error" to "invalid token payload")
                )

                val postId = parsePostId(call.pathParameters["id"]) ?: return@post call.respond(
                    status = HttpStatusCode.BadRequest, message = mapOf("error" to "invalid post id")
                )

                insertLike(userId, postId).onSuccess { result ->
                    when (result) {
                        null -> call.respond(
                            status = HttpStatusCode.NotFound, message = mapOf("error" to "post not found")
                        )

                        else -> call.respond(
                            status = when {
                                result.alreadyLiked -> HttpStatusCode.OK
                                else -> HttpStatusCode.Created
                            }, message = result
                        )
                    }
                }.onFailure {
                    call.respond(
                        status = HttpStatusCode.InternalServerError, message = mapOf("error" to "internal server error")
                    )
                }
            }
        }
    }
}