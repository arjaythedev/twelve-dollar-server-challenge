package dev.noamm9.velox

import dev.noamm9.velox.plugins.*
import dev.noamm9.velox.utils.HOST
import dev.noamm9.velox.utils.PORT
import io.ktor.server.cio.*
import io.ktor.server.engine.*

fun main() {
    embeddedServer(factory = CIO, host = HOST, port = PORT, module = {
        configureDatabase()
        configureSerialization()
        configureStatusPages()
        configureRouting()
    }).start(wait = true)
}
