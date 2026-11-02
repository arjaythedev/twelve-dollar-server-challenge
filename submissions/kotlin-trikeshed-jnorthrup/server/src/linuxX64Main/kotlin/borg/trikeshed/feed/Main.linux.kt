package borg.trikeshed.feed

import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.toKString
import platform.posix.getenv

/** Configured by SQLITE_PATH, JWT_SECRET, HOST and PORT: the Xio states over io_uring direct descriptors. */
@OptIn(ExperimentalForeignApi::class)
fun main() {
    fun env(name: String): String? = getenv(name)?.toKString()
    val store = SqliteStore(env("SQLITE_PATH") ?: error("SQLITE_PATH is not set"))
    val feed = Feed(store, Bearer((env("JWT_SECRET") ?: error("JWT_SECRET is not set")).encodeToByteArray()), epochSeconds())
    Xio(Poller(env("HOST") ?: "127.0.0.1", (env("PORT") ?: "3000").toInt()), feed, store).init()
}
