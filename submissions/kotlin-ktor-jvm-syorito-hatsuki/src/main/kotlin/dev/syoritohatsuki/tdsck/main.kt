package dev.syoritohatsuki.tdsck

import dev.syoritohatsuki.tdsck.plugins.*
import dev.syoritohatsuki.tdsck.utils.HOST
import dev.syoritohatsuki.tdsck.utils.PORT
import io.ktor.server.cio.*
import io.ktor.server.engine.*

fun main() {
    embeddedServer(factory = CIO, host = HOST, port = PORT, module = {
        configureAuthentication()
        configureDatabase()
        configureSerialization()
        configureStatusPages()
        configureRouting()
    }).start(wait = true)
}
