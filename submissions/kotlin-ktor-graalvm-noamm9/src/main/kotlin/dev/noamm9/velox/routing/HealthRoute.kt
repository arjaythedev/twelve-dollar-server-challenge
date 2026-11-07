package dev.noamm9.velox.routing

import dev.noamm9.velox.plugins.health
import io.ktor.http.HttpStatusCode
import io.ktor.server.response.respond
import io.ktor.server.routing.Route
import io.ktor.server.routing.get
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

val START_TIME = System.currentTimeMillis()

@Serializable
data class Health(
    val status: String,
    val db: String,
    @SerialName("uptime_s") val uptimeSeconds: Long? = null,
    val error: String? = null
)

fun Route.healthRoute() {
    get("health") {
        health().onSuccess {
            call.respond(
                HttpStatusCode.OK, Health(
                    status = "ok",
                    db = "ok",
                    uptimeSeconds = (System.currentTimeMillis() - START_TIME) / 1000
                )
            )
        }.onFailure {
            call.respond(
                HttpStatusCode.ServiceUnavailable, Health(
                    status = "degraded",
                    db = "unreachable",
                    error = it.localizedMessage
                )
            )
        }
    }
}