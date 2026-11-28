@file:OptIn(ExperimentalForeignApi::class)

package app

import kotlinx.cinterop.*
import knc.*
import platform.posix.exit
import platform.posix.getenv

private fun env(name: String): String? = getenv(name)?.toKString()

fun main() {
    val path = env("SQLITE_PATH") ?: error("SQLITE_PATH is not set")
    val secret = env("JWT_SECRET") ?: error("JWT_SECRET is not set")
    val host = env("HOST") ?: "0.0.0.0"
    val port = (env("PORT") ?: "3000").toInt()
    // KN_VFS: SQLite VFS for all connections (default "unix"; "unix-excl" = this process holds the file lock).
    // KN_BTREE=0: SQL only. KN_VERIFY=1: compare the B-tree paths with SQL for every post id and exit.
    val vfs = env("KN_VFS")?.takeIf { it.isNotEmpty() }
    val btree = if (env("KN_BTREE") == "0") 0 else 1

    memScoped {
        val err = allocArray<ByteVar>(512)
        if (db_open(path, vfs, btree, err, 512) != 0) {
            println("db_open: ${err.toKString()}")
            exit(1)
        }
    }
    if (env("KN_VERIFY") == "1") exit(if (db_verify() == 0) 0 else 1)
    if (ckpt_start() != 0) println("checkpointer thread not started")
    if (ur_init(host, port, envLong("KN_RINGS", 8).toInt()) != 0) exit(1)
    // The committer thread (KN_COMMITTER=1, or the slow-disk fallback) wakes the loop through this eventfd (ring 0).
    val wakeFd = db_wq_init()
    if (wakeFd < 0 || ur_watch_eventfd(wakeFd) != 0) { println("committer thread not started"); exit(1) }
    println("listening on $host:$port (vfs ${vfs ?: "unix"}, btree reads ${if (btree == 1) "on" else "off"})")
    Server(Jwt(secret)).run()
}
