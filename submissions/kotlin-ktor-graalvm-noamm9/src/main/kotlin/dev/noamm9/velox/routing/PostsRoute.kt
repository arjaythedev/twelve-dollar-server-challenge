package dev.noamm9.velox.routing

import dev.noamm9.velox.plugins.*
import io.ktor.http.HttpStatusCode
import io.ktor.server.request.receiveText
import io.ktor.server.response.respond
import io.ktor.server.routing.*
import kotlinx.serialization.SerializationException
import kotlinx.serialization.json.*

private const val JS_WHITESPACES = "\t\n\u000B\u000C\r \u00A0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029\u202F\u205F\u3000\uFEFF"

private fun parsePostId(raw: String?) = raw?.toIntOrNull()?.takeIf { it > 0 && it < Int.MAX_VALUE }
private fun String.trimJsWhitespace() = trim { it in JS_WHITESPACES }

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

        post {
            val user = call.authenticate() ?: return@post

            val json = try {
                Json.parseToJsonElement(call.receiveText())
            }
            catch (_: SerializationException) {
                return@post call.respond(
                    status = HttpStatusCode.BadRequest,
                    message = mapOf("error" to "malformed JSON body")
                )
            }
            catch (_: IllegalArgumentException) {
                return@post call.respond(
                    status = HttpStatusCode.BadRequest,
                    message = mapOf("error" to "malformed JSON body")
                )
            }

            val body =
                ((json as? JsonObject)?.get("body") as? JsonPrimitive)?.takeIf { it.isString }?.jsonPrimitive?.content?.trimJsWhitespace()
                    ?: return@post call.respond(
                        status = HttpStatusCode.BadRequest,
                        message = mapOf("error" to "body is required")
                    )

            if (body.isEmpty()) {
                return@post call.respond(
                    status = HttpStatusCode.BadRequest,
                    message = mapOf("error" to "body is required")
                )
            }

            if (body.length > 500) {
                return@post call.respond(
                    status = HttpStatusCode.BadRequest,
                    message = mapOf("error" to "body must be at most 500 characters")
                )
            }

            insertPost(user.userId, user.username, body).onSuccess { post ->
                call.respond(
                    status = HttpStatusCode.Created,
                    message = mapOf("post" to post)
                )
            }.onFailure {
                call.respond(
                    status = HttpStatusCode.InternalServerError,
                    message = mapOf("error" to "internal server error")
                )
            }
        }

        post("{id}/like") {
            val user = call.authenticate() ?: return@post

            val postId = parsePostId(call.pathParameters["id"]) ?: return@post call.respond(
                status = HttpStatusCode.BadRequest,
                message = mapOf("error" to "invalid post id")
            )

            insertLike(user.userId, postId).onSuccess { result ->
                when (result) {
                    null -> call.respond(
                        status = HttpStatusCode.NotFound,
                        message = mapOf("error" to "post not found")
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
                    status = HttpStatusCode.InternalServerError,
                    message = mapOf("error" to "internal server error")
                )
            }
        }
    }
}