package challenge.feed.health

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import org.springframework.beans.factory.annotation.Qualifier
import org.springframework.dao.DataAccessException
import org.springframework.http.HttpStatus
import org.springframework.http.ResponseEntity
import org.springframework.jdbc.core.simple.JdbcClient
import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.RestController
import java.time.Duration

/** Null fields are omitted (`explicit-nulls=false`), giving the spec's two shapes. */
@Serializable
data class HealthStatus(
    val status: String,
    val db: String,
    @SerialName("uptime_s") val uptimeS: Long? = null,
    val error: String? = null,
) {
    companion object {
        fun up(uptimeS: Long) = HealthStatus("ok", "ok", uptimeS = uptimeS)
        fun down(error: String) = HealthStatus("degraded", "unreachable", error = error)
    }
}

@RestController
class HealthController(@Qualifier("readerJdbc") private val db: JdbcClient) {
    private val startedAt = System.nanoTime()

    @GetMapping("/health")
    fun health(): ResponseEntity<HealthStatus> =
        try {
            db.sql("SELECT 1").query(Int::class.javaObjectType).single()
            ResponseEntity.ok(HealthStatus.up(Duration.ofNanos(System.nanoTime() - startedAt).toSeconds()))
        } catch (e: DataAccessException) {
            ResponseEntity.status(HttpStatus.SERVICE_UNAVAILABLE).body(HealthStatus.down(e.mostSpecificCause.message ?: e.toString()))
        }
}
