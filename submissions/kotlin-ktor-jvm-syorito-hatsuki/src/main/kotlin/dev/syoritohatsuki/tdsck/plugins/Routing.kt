package dev.syoritohatsuki.tdsck.plugins

import dev.syoritohatsuki.tdsck.routing.feedRoute
import dev.syoritohatsuki.tdsck.routing.healthRoute
import dev.syoritohatsuki.tdsck.routing.postsRoute
import io.ktor.http.*
import io.ktor.server.application.*
import io.ktor.server.response.*
import io.ktor.server.routing.*

fun Application.configureRouting() {
    routing {
        healthRoute()
        feedRoute()
        postsRoute()

        route("{...}") {
            handle {
                call.respond(
                    status = HttpStatusCode.NotFound,
                    message = mapOf("error" to "not found")
                )
            }
        }
    }
}