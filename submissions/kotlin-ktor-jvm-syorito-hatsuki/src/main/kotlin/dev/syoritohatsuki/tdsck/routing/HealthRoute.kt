package dev.syoritohatsuki.tdsck.routing

import dev.syoritohatsuki.tdsck.plugins.health
import io.ktor.http.*
import io.ktor.server.response.*
import io.ktor.server.routing.*
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

val UPTIME = System.currentTimeMillis()

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
                    status = "ok", db = "ok", uptimeSeconds = (System.currentTimeMillis() - UPTIME) / 1000
                )
            )
        }.onFailure {
            call.respond(
                HttpStatusCode.ServiceUnavailable, Health(
                    status = "degraded", db = "unreachable", error = it.localizedMessage
                )
            )
        }
    }
}