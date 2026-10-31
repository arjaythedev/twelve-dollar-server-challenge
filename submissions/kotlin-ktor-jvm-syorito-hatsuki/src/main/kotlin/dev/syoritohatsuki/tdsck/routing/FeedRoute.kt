package dev.syoritohatsuki.tdsck.routing

import dev.syoritohatsuki.tdsck.plugins.Post
import dev.syoritohatsuki.tdsck.plugins.getFeed
import io.ktor.http.*
import io.ktor.server.response.*
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