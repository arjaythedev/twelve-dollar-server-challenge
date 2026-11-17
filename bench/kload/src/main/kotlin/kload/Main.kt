package kload

import java.io.File
import java.lang.management.ManagementFactory
import kotlin.system.exitProcess

fun main(args: Array<String>) {
    val cfg = try { Config.parse(args) } catch (e: Exception) { System.err.println(e.message); exitProcess(2) }
    val t0 = System.nanoTime() + 200_000_000L

    // Schedule exactly like k6's ramping-vus: VU i starts at ramp*i/N; during ramp-down the highest-numbered VUs
    // are deactivated first, each with gracefulRampDown to finish its iteration.
    val perWorker = List(cfg.threads) { ArrayList<Vu>(cfg.vus / cfg.threads + 1) }
    for (i in 0 until cfg.vus) {
        val start = t0 + (cfg.rampNs.toDouble() * i / cfg.vus).toLong()
        val stop = t0 + cfg.rampNs + cfg.holdNs + (cfg.rampDownNs.toDouble() * (cfg.vus - 1 - i) / cfg.vus).toLong()
        val v = Vu(
            num = i + 1,
            tok = cfg.tokens[i % cfg.tokens.size],
            target = cfg.targets[i % cfg.targets.size],
            src = if (cfg.sourceIps.isEmpty()) null else cfg.sourceIps[i % cfg.sourceIps.size],
            srcIdx = if (cfg.sourceIps.isEmpty()) 0 else i % cfg.sourceIps.size,
            startAt = start, stopAt = stop, hardStop = stop + cfg.graceNs,
        )
        perWorker[i % cfg.threads].add(v)
    }
    val portCursors = java.util.concurrent.atomic.AtomicIntegerArray(maxOf(1, cfg.sourceIps.size))
    val workers = perWorker.mapIndexed { idx, vus -> Worker(idx, cfg, t0, vus, portCursors) }
    val threads = workers.mapIndexed { idx, w -> Thread(w, "kload-$idx").apply { start() } }

    println("kload: ${cfg.vus} VUs -> ${cfg.targets.joinToString { it.text }} | ramp ${s(cfg.rampNs)} hold ${s(cfg.holdNs)} " +
        "ramp-down ${s(cfg.rampDownNs)} | ${cfg.threads} threads" +
        (if (cfg.sourceIps.isNotEmpty()) " | source IPs ${cfg.sourceIps.first().hostAddress}..${cfg.sourceIps.last().hostAddress} (${cfg.sourceIps.size})" else ""))

    var maxOpen = 0L
    var lastPrint = System.nanoTime()
    var prevAll = Hist(); var prevReqs = 0L; var prevFails = 0L; var prevConn = 0L; var prevIn = 0L; var prevOut = 0L
    while (threads.any { it.isAlive }) {
        threads.first { it.isAlive }.join(1000)
        val open = workers.sumOf { it.st.open }
        if (open > maxOpen) maxOpen = open
        val now = System.nanoTime()
        if (!cfg.quiet && now - lastPrint >= 10_000_000_000L) {
            val dt = (now - lastPrint) / 1e9
            lastPrint = now
            val all = Hist().also { h -> workers.forEach { h.merge(it.st.all.snapshot()) } }
            val reqs = workers.sumOf { it.st.reqs.sum() }
            val fails = workers.sumOf { it.st.fails.sum() }
            val conns = workers.sumOf { it.st.connAttempts }
            val bin = workers.sumOf { it.st.bytesIn }; val bout = workers.sumOf { it.st.bytesOut }
            val iv = all.minus(prevAll)
            val active = workers.sumOf { it.st.started - it.st.finished }
            println(String.format(
                "[%5.0fs] vus %6d  open %6d | %7.0f req/s  fail %5.2f%% | p95 %7s  p99 %7s | +%5.0f conn/s | in %6.1f Mbit/s  out %5.1f Mbit/s",
                (now - t0) / 1e9, active, open, (reqs - prevReqs) / dt,
                if (reqs > prevReqs) (fails - prevFails) * 100.0 / (reqs - prevReqs) else 0.0,
                ms(iv.pct(0.95)), ms(iv.pct(0.99)), (conns - prevConn) / dt,
                (bin - prevIn) * 8 / dt / 1e6, (bout - prevOut) * 8 / dt / 1e6))
            prevAll = all; prevReqs = reqs; prevFails = fails; prevConn = conns; prevIn = bin; prevOut = bout
        }
    }
    val wall = (System.nanoTime() - t0) / 1e9
    val st = Stats().also { s -> workers.forEach { s.merge(it.st) } }
    exitProcess(report(cfg, st, wall, maxOpen))
}

private fun s(ns: Long) = if (ns % 60_000_000_000L == 0L && ns > 0) "${ns / 60_000_000_000L}m" else "${ns / 1_000_000_000L}s"
private fun ms(us: Double) = if (us >= 10_000) String.format("%.0fms", us / 1000) else String.format("%.1fms", us / 1000)

private fun procStatus(key: String): Long =
    File("/proc/self/status").readLines().firstOrNull { it.startsWith("$key:") }?.split(Regex("\\s+"))?.get(1)?.toLong() ?: 0L

private fun report(cfg: Config, st: Stats, wall: Double, maxOpen: Long): Int {
    val total = st.reqs.sum()
    val failed = st.fails.sum()
    val failRate = if (total > 0) failed.toDouble() / total else 0.0
    val a = st.all
    val p95 = a.pct(0.95) / 1000; val p99 = a.pct(0.99) / 1000
    val ok95 = p95 < 500; val ok99 = p99 < 1000; val okFail = failRate < 0.01
    val pass = ok95 && ok99 && okFail
    val rssMb = procStatus("VmRSS") / 1024.0
    val hwmMb = procStatus("VmHWM") / 1024.0
    val cpu = (ManagementFactory.getOperatingSystemMXBean() as com.sun.management.OperatingSystemMXBean).processCpuTime / 1e9
    val holdS = cfg.holdNs / 1e9

    println()
    println("  http_req_duration (whole run, as k6 thresholds see it)")
    println(String.format("    avg %s  med %s  p90 %s  p95 %s  p99 %s  max %s", ms(a.avg()), ms(a.pct(0.5)), ms(a.pct(0.9)), ms(a.pct(0.95)), ms(a.pct(0.99)), ms(a.max.toDouble())))
    println(String.format("  requests      %d (%.0f/s)   failed %d (%.3f%%)   iterations %d", total, total / wall, failed, failRate * 100, st.iterations))
    println(String.format("  hold window   %d reqs (%.0f/s)   failed %.3f%%   p95 %s  p99 %s",
        st.holdReqs, st.holdReqs / holdS, if (st.holdReqs > 0) st.holdFails * 100.0 / st.holdReqs else 0.0, ms(st.hold.pct(0.95)), ms(st.hold.pct(0.99))))
    println("  by endpoint")
    for (k in 0 until 4) {
        val h = st.byKind[k]
        println(String.format("    %-22s %9d  failed %7d  p95 %8s  p99 %8s  max %8s", KIND_NAMES[k], st.reqs[k], st.fails[k], ms(h.pct(0.95)), ms(h.pct(0.99)), ms(h.max.toDouble())))
    }
    println(String.format("  connections   %d attempts (%.0f/s)   failed %d (bind/port %d)   timeouts %d   keep-alive retries %d   max open %d",
        st.connAttempts, st.connAttempts / wall, st.connFails, st.bindFails, st.timeouts, st.retries, maxOpen))
    println(String.format("  connect time  med %s  p99 %s  max %s   (not part of http_req_duration, same as k6)",
        ms(st.connect.pct(0.5)), ms(st.connect.pct(0.99)), ms(st.connect.max.toDouble())))
    println(String.format("  network       in %.1f MB (%.1f Mbit/s avg)   out %.1f MB (%.1f Mbit/s avg)",
        st.bytesIn / 1e6, st.bytesIn * 8 / wall / 1e6, st.bytesOut / 1e6, st.bytesOut * 8 / wall / 1e6))
    println(String.format("  generator     %d threads   cpu %.0fs (%.0f%% of one core)   rss %.0f MB (peak %.0f MB, %.1f KB/VU)",
        cfg.threads, cpu, cpu / wall * 100, rssMb, hwmMb, hwmMb * 1024 / cfg.vus))
    println(String.format("  self-check    longest event-loop pass %s, passes over 100ms: %d%s",
        ms(st.maxLoopNs / 1000.0), st.loopStalls,
        if (st.loopStalls > 0) "  <- the GENERATOR stalled; latency numbers are unreliable" else " (latency is the server's, not kload's)"))
    println()
    println("  thresholds    p(95)<500ms ${if (ok95) "✓" else "✗"} ${ms(p95 * 1000)}   p(99)<1000ms ${if (ok99) "✓" else "✗"} ${ms(p99 * 1000)}   " +
        "http_req_failed<1% ${if (okFail) "✓" else "✗"} ${String.format("%.3f%%", failRate * 100)}   =>  ${if (pass) "PASS" else "FAIL"}")

    cfg.jsonOut?.let { path ->
        val kinds = (0 until 4).joinToString(",") { k ->
            val h = st.byKind[k]
            "\"${KIND_NAMES[k]}\":{\"reqs\":${st.reqs[k]},\"failed\":${st.fails[k]},\"p95_ms\":${h.pct(0.95) / 1000},\"p99_ms\":${h.pct(0.99) / 1000}}"
        }
        File(path).writeText(
            "{\"vus\":${cfg.vus},\"pass\":$pass,\"reqs\":$total,\"failed\":$failed,\"fail_rate\":$failRate," +
                "\"med_ms\":${a.pct(0.5) / 1000},\"p95_ms\":$p95,\"p99_ms\":$p99,\"max_ms\":${a.max / 1000.0}," +
                "\"hold\":{\"reqs\":${st.holdReqs},\"failed\":${st.holdFails},\"p95_ms\":${st.hold.pct(0.95) / 1000},\"p99_ms\":${st.hold.pct(0.99) / 1000}}," +
                "\"endpoints\":{$kinds},\"conn_attempts\":${st.connAttempts},\"conn_failed\":${st.connFails},\"timeouts\":${st.timeouts}," +
                "\"connect_p99_ms\":${st.connect.pct(0.99) / 1000},\"max_open\":$maxOpen,\"bytes_in\":${st.bytesIn},\"bytes_out\":${st.bytesOut}," +
                "\"gen_cpu_s\":$cpu,\"gen_rss_peak_mb\":$hwmMb,\"gen_loop_stalls\":${st.loopStalls},\"gen_max_loop_ms\":${st.maxLoopNs / 1e6},\"wall_s\":$wall}\n")
    }
    return if (pass) 0 else 99
}
