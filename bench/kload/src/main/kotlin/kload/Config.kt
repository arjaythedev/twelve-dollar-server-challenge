package kload

import java.io.File
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.URI

class Token(val username: String, val token: String) {
    val authLine: ByteArray = "Authorization: Bearer $token\r\nContent-Type: application/json\r\n".toByteArray()
}

class Target(url: String) {
    val addr: InetSocketAddress
    val hostLine: ByteArray
    val prefix: ByteArray
    val text = url.trimEnd('/')

    init {
        val u = URI(text)
        require(u.scheme == "http") { "only http:// targets are supported: $url" }
        val port = if (u.port > 0) u.port else 80
        addr = InetSocketAddress(InetAddress.getByName(u.host), port)
        hostLine = "Host: ${u.host}${if (u.port > 0) ":${u.port}" else ""}\r\nUser-Agent: kload/1.0\r\n".toByteArray()
        prefix = (u.rawPath ?: "").trimEnd('/').toByteArray()
    }
}

class Config(
    val targets: List<Target>,
    val vus: Int,
    val rampNs: Long,
    val holdNs: Long,
    val rampDownNs: Long,
    val graceNs: Long,
    val tokens: List<Token>,
    val threads: Int,
    val sourceIps: List<InetAddress>,
    val timeoutNs: Long,
    val likeProb: Double,
    val postProb: Double,
    val jsonOut: String?,
    val quiet: Boolean,
) {
    val totalNs get() = rampNs + holdNs + rampDownNs + graceNs

    companion object {
        private const val USAGE = """kload: k6-compatible replay of the twelve-dollar-server-challenge bench/load.js, in Kotlin.

usage: kload [options]            (k6 env vars BASE_URL, VUS, DURATION, RAMP_UP, TOKENS are honoured too)
  --url URL[,URL...]     target(s); VUs are spread round-robin    (default http://127.0.0.1:3000)
  --vus N                virtual users                            (default 100)
  --ramp-up D            ramp 0 -> N                               (default 60s)
  --duration D           hold at N                                 (default 5m)
  --ramp-down D          ramp N -> 0                               (default 30s)
  --grace D              graceful ramp-down per VU                 (default 10s)
  --tokens FILE          seed/tokens.json                          (default: ../seed or ../../seed/tokens.json)
  --threads N            event-loop threads                        (default: min(CPUs, 4))
  --source-ips SPEC      bind sockets to these local IPs, e.g. 10.0.0.5,10.0.0.6 or 127.0.0.1-127.0.0.8
                         (default for a loopback target: one 127.0.0.x per 25k VUs)
  --timeout D            request timeout                           (default 60s)
  --json FILE            also write the summary as JSON
  --quiet                no progress lines
durations: 500ms, 30s, 5m, 1h"""

        fun dur(s: String): Long {
            val m = Regex("^(\\d+(?:\\.\\d+)?)(ms|s|m|h)$").matchEntire(s.trim()) ?: error("bad duration '$s'")
            val v = m.groupValues[1].toDouble()
            val mult = when (m.groupValues[2]) { "ms" -> 1e6; "s" -> 1e9; "m" -> 60e9; else -> 3600e9 }
            return (v * mult).toLong()
        }

        fun ips(spec: String): List<InetAddress> = spec.split(',').filter { it.isNotBlank() }.flatMap { part ->
            val r = part.trim().split('-')
            if (r.size == 1) listOf(InetAddress.getByName(r[0]))
            else {
                val a = InetAddress.getByName(r[0]).address
                val b = InetAddress.getByName(r[1]).address
                require(a.size == 4 && a.copyOf(3).contentEquals(b.copyOf(3))) { "IP ranges must differ only in the last octet: $part" }
                ((a[3].toInt() and 0xFF)..(b[3].toInt() and 0xFF)).map { InetAddress.getByAddress(byteArrayOf(a[0], a[1], a[2], it.toByte())) }
            }
        }

        fun loadTokens(path: String): List<Token> {
            val text = File(path).readText()
            val user = Regex("\"username\"\\s*:\\s*\"([^\"]*)\"")
            val tok = Regex("\"token\"\\s*:\\s*\"([^\"]*)\"")
            val out = Regex("\\{[^{}]*\\}").findAll(text).mapNotNull { m ->
                val u = user.find(m.value)?.groupValues?.get(1) ?: return@mapNotNull null
                val t = tok.find(m.value)?.groupValues?.get(1) ?: return@mapNotNull null
                Token(u, t)
            }.toList()
            require(out.isNotEmpty()) { "no {username, token} objects in $path" }
            return out
        }

        fun parse(args: Array<String>): Config {
            val env = System.getenv()
            val o = HashMap<String, String>()
            var i = 0
            while (i < args.size) {
                val a = args[i]
                when {
                    a == "-h" || a == "--help" -> { println(USAGE); kotlin.system.exitProcess(0) }
                    a == "--quiet" -> o["quiet"] = "1"
                    a.startsWith("--") && a.contains('=') -> o[a.substring(2, a.indexOf('='))] = a.substringAfter('=')
                    a.startsWith("--") && i + 1 < args.size -> { o[a.substring(2)] = args[i + 1]; i++ }
                    else -> error("unknown argument '$a'\n\n$USAGE")
                }
                i++
            }
            val urls = (o["url"] ?: env["BASE_URL"] ?: "http://127.0.0.1:3000").split(',').map { Target(it) }
            val vus = (o["vus"] ?: env["VUS"] ?: "100").toInt()
            val cpus = Runtime.getRuntime().availableProcessors()
            var src = o["source-ips"]?.let { ips(it) } ?: emptyList()
            if (src.isEmpty() && urls.all { it.addr.address.isLoopbackAddress }) {
                // One loopback source IP per 25k VUs keeps every (src IP, dst) pair under the ~28k ephemeral ports.
                val n = (vus + 24_999) / 25_000
                if (n > 1) src = (1..n).map { InetAddress.getByAddress(byteArrayOf(127, 0, 0, it.toByte())) }
            }
            return Config(
                targets = urls,
                vus = vus,
                rampNs = dur(o["ramp-up"] ?: env["RAMP_UP"] ?: "60s"),
                holdNs = dur(o["duration"] ?: env["DURATION"] ?: "5m"),
                rampDownNs = dur(o["ramp-down"] ?: "30s"),
                graceNs = dur(o["grace"] ?: "10s"),
                tokens = loadTokens(o["tokens"] ?: env["TOKENS"]
                    ?: listOf("../seed/tokens.json", "../../seed/tokens.json", "seed/tokens.json").firstOrNull { File(it).exists() }
                    ?: "../seed/tokens.json"),
                threads = (o["threads"]?.toInt() ?: minOf(cpus, 4)).coerceIn(1, maxOf(1, vus)),
                sourceIps = src,
                timeoutNs = dur(o["timeout"] ?: "60s"),
                likeProb = (o["like-prob"] ?: "0.15").toDouble(),
                postProb = (o["post-prob"] ?: "0.02").toDouble(),
                jsonOut = o["json"],
                quiet = o.containsKey("quiet"),
            )
        }
    }
}
