package nio

import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.Linker
import java.lang.foreign.ValueLayout.JAVA_INT
import java.sql.DriverManager

/**
 * Keeps the WAL short, off the response path, on its own connection (worker 0 only; the WAL is shared).
 *
 * Every TICK_MS it runs a PASSIVE checkpoint, which copies whatever no reader still needs and never waits.
 * Once the log holds RESTART_FRAMES or more and PASSIVE has copied all but TAIL_FRAMES of it, it runs a RESTART
 * checkpoint with a busy timeout of HOLD_MS. RESTART takes SQLite's WAL write lock, so it holds the writers of
 * *both* worker processes (their BEGIN IMMEDIATE fails at once with busy_timeout=0, and they retry ~1 ms later
 * while reads go on), copies the short tail, and waits for readers to leave the old frames; the next write then
 * starts the WAL from frame 1. Without this, two processes always have a reader in the WAL, it never resets, and
 * every page lookup gets slower as the WAL grows (measured: feed 27 -> 45 us, post 4.7 -> 8.7 us).
 *
 * The thread runs at nice 5 (setpriority on its own tid through FFM). At nice 19 it starves under saturation,
 * at nice 0 it competes with the event loop as an equal; 5 gets it about a quarter of a contended core.
 * Credit (MIT): the PASSIVE-then-bounded-hold cycle and nice 5 follow swift-iouring-akbashev
 * (Sources/Feed/Checkpointer.swift) and rust-iouring-kensac's checkpoint state machine.
 * This connection never commits anything, so it never invalidates the loop connections' page caches.
 */
class Checkpointer(private val path: String) : Thread("checkpointer") {
    init { isDaemon = true }

    companion object {
        private const val TICK_MS = 50L
        private const val RESTART_FRAMES = 4096   // 16 MB of 4 KB pages; about 1 s of writes at 128k users
        private const val TAIL_FRAMES = 128
        private const val HOLD_MS = 250
        private val LOG = System.getenv("KNIO_CKPT_LOG") == "1"   // stats every 10 s on stderr

        /** setpriority(PRIO_PROCESS, gettid(), n): on Linux this re-nices only the calling thread. */
        fun niceThisThread(n: Int): Boolean = try {
            val linker = Linker.nativeLinker()
            val lookup = linker.defaultLookup()
            val gettid = linker.downcallHandle(lookup.find("gettid").orElseThrow(), FunctionDescriptor.of(JAVA_INT))
            val setpriority = linker.downcallHandle(
                lookup.find("setpriority").orElseThrow(), FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT)
            )
            val tid = gettid.invokeExact() as Int
            (setpriority.invokeExact(0, tid, n) as Int) == 0
        } catch (e: Throwable) {
            System.err.println("checkpointer: could not set nice $n: $e"); false
        }
    }

    override fun run() {
        val niced = niceThisThread(5)
        val c = DriverManager.getConnection("jdbc:sqlite:$path", Db.config(busyMs = HOLD_MS).toProperties())
        val st = c.createStatement()
        var restarts = 0; var restartFails = 0; var maxHoldNs = 0L; var sumHoldNs = 0L; var maxLog = 0
        var lastLog = System.nanoTime()
        if (LOG) System.err.println("checkpointer: nice5=$niced tick=${TICK_MS}ms restart>=${RESTART_FRAMES} frames")
        while (true) {
            try {
                sleep(TICK_MS)
                var log = 0; var copied = 0
                st.executeQuery("PRAGMA wal_checkpoint(PASSIVE)").use { if (it.next()) { log = it.getInt(2); copied = it.getInt(3) } }
                if (log > maxLog) maxLog = log
                if (log >= RESTART_FRAMES && log - copied <= TAIL_FRAMES) {
                    val t0 = System.nanoTime()
                    var busy = 1
                    st.executeQuery("PRAGMA wal_checkpoint(RESTART)").use { if (it.next()) busy = it.getInt(1) }
                    val dt = System.nanoTime() - t0
                    if (busy == 0) restarts++ else restartFails++
                    sumHoldNs += dt; if (dt > maxHoldNs) maxHoldNs = dt
                }
                if (LOG && System.nanoTime() - lastLog > 10_000_000_000L) {
                    System.err.println(
                        "checkpointer: restarts=$restarts busy=$restartFails maxLog=$maxLog frames " +
                            "hold avg=${if (restarts + restartFails > 0) sumHoldNs / (restarts + restartFails) / 1000 else 0}us max=${maxHoldNs / 1000}us"
                    )
                    restarts = 0; restartFails = 0; maxHoldNs = 0; sumHoldNs = 0; maxLog = 0; lastLog = System.nanoTime()
                }
            } catch (e: InterruptedException) {
                return
            } catch (e: Exception) {
                System.err.println("checkpoint: $e")
            }
        }
    }
}
