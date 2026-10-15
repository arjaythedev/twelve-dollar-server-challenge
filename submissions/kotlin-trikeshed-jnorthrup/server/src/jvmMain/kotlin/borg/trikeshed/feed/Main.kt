package borg.trikeshed.feed

/** Configured only by SQLITE_PATH, JWT_SECRET, HOST and PORT. */
fun main() {
    val env = System.getenv()
    val store = SqliteStore(env.getValue("SQLITE_PATH"))
    val feed = Feed(store, Bearer(env.getValue("JWT_SECRET").encodeToByteArray()), epochSeconds())
    Xio(Poller(env["HOST"] ?: "127.0.0.1", (env["PORT"] ?: "3000").toInt()), feed, store).init()
}
