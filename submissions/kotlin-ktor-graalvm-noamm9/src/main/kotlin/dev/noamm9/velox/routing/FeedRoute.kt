package dev.noamm9.velox.routing

import dev.noamm9.velox.plugins.Post
import dev.noamm9.velox.plugins.getFeed
import io.ktor.http.HttpStatusCode
import io.ktor.server.response.respond
import io.ktor.server.routing.*
import kotlinx.serialization.Serializable

@Serializable
data class Feed(val posts: List<Post> = emptyList())

fun Route.feedRoute() {
    route("/feed") {
        get {
            getFeed().onSuccess {
                call.respond(Feed(it))
            }.onFailure {
                call.respond(
                    status = HttpStatusCode.InternalServerError,
                    message = mapOf("error" to (it.message ?: "internal server error"))
                )
            }
        }
    }
}