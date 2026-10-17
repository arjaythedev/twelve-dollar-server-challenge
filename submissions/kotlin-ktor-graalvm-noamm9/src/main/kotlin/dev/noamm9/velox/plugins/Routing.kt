package dev.noamm9.velox.plugins

import dev.noamm9.velox.routing.feedRoute
import dev.noamm9.velox.routing.healthRoute
import dev.noamm9.velox.routing.postsRoute
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