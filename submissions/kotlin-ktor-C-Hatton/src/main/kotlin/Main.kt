package server

import io.ktor.http.ContentType
import io.ktor.http.HttpMethod
import io.ktor.http.HttpStatusCode
import io.ktor.server.application.Application
import io.ktor.server.application.ApplicationCall
import io.ktor.server.application.ApplicationCallPipeline
import io.ktor.server.application.call
import io.ktor.server.engine.connector
import io.ktor.server.engine.embeddedServer
import io.ktor.server.netty.Netty
import io.ktor.server.request.httpMethod
import io.ktor.server.request.path
import io.ktor.server.request.receive
import io.ktor.server.response.respondBytes
import kotlinx.coroutines.CancellationException
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull
import java.security.MessageDigest
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

private val startedAt = System.nanoTime()
private val JSON = ContentType.Application.Json
private val CREATED = HttpStatusCode.Created
private val jwtKey = SecretKeySpec((System.getenv("JWT_SECRET") ?: "").toByteArray(), "HmacSHA256")

private fun err(msg: String) = """{"error":"$msg"}""".toByteArray()
private val NOT_FOUND = err("not found")
private val INVALID_ID = err("invalid post id")
private val POST_NOT_FOUND = err("post not found")
private val MISSING_TOKEN = err("missing bearer token")
private val INVALID_TOKEN = err("invalid or expired token")
private val INVALID_PAYLOAD = err("invalid token payload")
private val MALFORMED = err("malformed JSON body")
private val BODY_REQUIRED = err("body is required")
private val BODY_TOO_LONG = err("body must be at most 500 characters")
private val INTERNAL = err("internal server error")

/** Per-thread state: the SQLite connection with its prepared statements, an HMAC, and an output buffer. */
private class Worker {
    val db = Db(System.getenv("SQLITE_PATH") ?: error("SQLITE_PATH is not set"))
    val mac: Mac = Mac.getInstance("HmacSHA256").apply { init(jwtKey) }
    val out = JsonOut()
}

private val workers = ThreadLocal.withInitial { Worker() }

fun main() {
    val host = System.getenv("HOST") ?: "0.0.0.0"
    val port = (System.getenv("PORT") ?: "80").toInt()
    val connections = IdleConnections.forFileLimit()
    embeddedServer(Netty, configure = {
        connector { this.host = host; this.port = port }
        // One vCPU: one event-loop thread reads, parses, runs the handlers (which call SQLite inline) and
        // writes; another only accepts. With shareWorkGroup the worker group gets workerGroupSize +
        // callGroupSize threads, so callGroupSize = 0 keeps it at one thread and one SQLite connection,
        // whose page cache then stays warm (a write on another connection would reset it).
        connectionGroupSize = 1
        workerGroupSize = 1
        callGroupSize = 0
        shareWorkGroup = true
        requestReadTimeoutSeconds = 0
        // Responses are a few KB and fit in the socket buffer: no write timeout, so the (then pass-through)
        // timeout handler is dropped from each connection's pipeline.
        responseWriteTimeoutSeconds = 0
        tcpKeepAlive = false
        channelPipelineConfig = {
            remove("timeout")
            addAfter("codec", "idle", connections.handler())
        }
    }, module = Application::module).start(wait = true)
}

fun Application.module() {
    intercept(ApplicationCallPipeline.Call) {
        try {
            route(call)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Throwable) {
            System.err.println("request failed: $e")
            call.respondBytes(INTERNAL, JSON, HttpStatusCode.InternalServerError)
        }
    }
}

private suspend fun route(call: ApplicationCall) {
    val path = call.request.path()
    val get = call.request.httpMethod == HttpMethod.Get
    val post = call.request.httpMethod == HttpMethod.Post
    when {
        get && path == "/feed" -> {
            val w = workers.get()
            call.respondBytes(w.db.feed(w.out), JSON)
        }
        path.startsWith("/posts/") -> {
            val rest = path.substring(7)
            if (get) {
                if (rest.indexOf('/') >= 0) return call.respondBytes(NOT_FOUND, JSON, HttpStatusCode.NotFound)
                val id = parseId(rest) ?: return call.respondBytes(INVALID_ID, JSON, HttpStatusCode.BadRequest)
                val w = workers.get()
                val body = w.db.post(id, w.out)
                if (body == null) call.respondBytes(POST_NOT_FOUND, JSON, HttpStatusCode.NotFound)
                else call.respondBytes(body, JSON)
            } else if (post && rest.endsWith("/like") && rest.indexOf('/') == rest.length - 5) {
                like(call, rest.substring(0, rest.length - 5))
            } else {
                call.respondBytes(NOT_FOUND, JSON, HttpStatusCode.NotFound)
            }
        }
        post && path == "/posts" -> create(call)
        get && path == "/health" -> health(call)
        else -> call.respondBytes(NOT_FOUND, JSON, HttpStatusCode.NotFound)
    }
}

/** A positive decimal integer, or null. Ids too large for a Long are valid ids that can't exist. */
private fun parseId(s: String): Long? {
    if (s.isEmpty()) return null
    var n = 0L
    for (c in s) {
        if (c !in '0'..'9') return null
        if (n > (Long.MAX_VALUE - 9) / 10) return Long.MAX_VALUE
        n = n * 10 + (c - '0')
    }
    return if (n > 0) n else null
}

private suspend fun health(call: ApplicationCall) {
    try {
        workers.get().db.ping()
        val up = (System.nanoTime() - startedAt) / 1_000_000_000
        call.respondBytes("""{"status":"ok","db":"ok","uptime_s":$up}""".toByteArray(), JSON)
    } catch (e: Exception) {
        val out = JsonOut()
        out.raw("""{"status":"degraded","db":"unreachable","error":""")
        out.str(e.message ?: e.toString())
        out.raw("}")
        call.respondBytes(out.toByteArray(), JSON, HttpStatusCode.ServiceUnavailable)
    }
}

private suspend fun like(call: ApplicationCall, idPart: String) {
    val w = workers.get()
    val user = authenticate(call, w) ?: return
    val id = parseId(idPart) ?: return call.respondBytes(INVALID_ID, JSON, HttpStatusCode.BadRequest)
    when (w.db.like(user.id, id)) {
        Db.LIKED -> call.respondBytes("""{"liked":true,"already_liked":false,"post_id":$id}""".toByteArray(), JSON, CREATED)
        Db.ALREADY -> call.respondBytes("""{"liked":true,"already_liked":true,"post_id":$id}""".toByteArray(), JSON)
        else -> call.respondBytes(POST_NOT_FOUND, JSON, HttpStatusCode.NotFound)
    }
}

private suspend fun create(call: ApplicationCall) {
    val w = workers.get()
    val user = authenticate(call, w) ?: return
    val raw = call.receive<ByteArray>()
    val json = try {
        Json.parseToJsonElement(raw.decodeToString(throwOnInvalidSequence = true))
    } catch (e: Exception) {
        return call.respondBytes(MALFORMED, JSON, HttpStatusCode.BadRequest)
    }
    val b = (json as? JsonObject)?.get("body") as? JsonPrimitive
    if (b == null || !b.isString) return call.respondBytes(BODY_REQUIRED, JSON, HttpStatusCode.BadRequest)
    val body = b.content.trim()
    if (body.isEmpty()) return call.respondBytes(BODY_REQUIRED, JSON, HttpStatusCode.BadRequest)
    if (body.codePointCount(0, body.length) > 500) return call.respondBytes(BODY_TOO_LONG, JSON, HttpStatusCode.BadRequest)
    call.respondBytes(w.db.create(user.id, user.name, body, w.out), JSON, CREATED)
}

private class User(val id: Long, val name: String)

/** Verifies the bearer JWT (HS256 signature, alg, exp, nbf) on every request. Responds 401 and returns null on failure. */
private suspend fun authenticate(call: ApplicationCall, w: Worker): User? {
    val header = call.request.headers["Authorization"]
    if (header == null || !header.startsWith("Bearer ")) {
        call.respondBytes(MISSING_TOKEN, JSON, HttpStatusCode.Unauthorized); return null
    }
    val token = header.substring(7)
    val claims = verify(token, w.mac)
    if (claims == null) {
        call.respondBytes(INVALID_TOKEN, JSON, HttpStatusCode.Unauthorized); return null
    }
    val sub = (claims["sub"] as? JsonPrimitive)?.takeIf { it.isString }?.content
    val name = (claims["username"] as? JsonPrimitive)?.takeIf { it.isString }?.content
    val id = sub?.let(::parseId)
    if (id == null || id == Long.MAX_VALUE || name == null) {
        call.respondBytes(INVALID_PAYLOAD, JSON, HttpStatusCode.Unauthorized); return null
    }
    return User(id, name)
}

private val b64 = Base64.getUrlDecoder()

private fun verify(token: String, mac: Mac): JsonObject? = try {
    val d1 = token.indexOf('.')
    val d2 = token.indexOf('.', d1 + 1)
    if (d1 <= 0 || d2 <= d1 + 1 || d2 == token.length - 1 || token.indexOf('.', d2 + 1) >= 0) null
    else {
        val head = Json.parseToJsonElement(b64.decode(token.substring(0, d1)).decodeToString()) as JsonObject
        val alg = head["alg"] as? JsonPrimitive
        if (alg == null || !alg.isString || alg.content != "HS256") null
        else {
            val expected = mac.doFinal(token.substring(0, d2).toByteArray(Charsets.US_ASCII))
            if (!MessageDigest.isEqual(expected, b64.decode(token.substring(d2 + 1)))) null
            else {
                val claims = Json.parseToJsonElement(b64.decode(token.substring(d1 + 1, d2)).decodeToString()) as JsonObject
                val now = System.currentTimeMillis() / 1000.0
                if (!timeOk(claims["exp"]) { now < it } || !timeOk(claims["nbf"]) { now >= it }) null else claims
            }
        }
    }
} catch (e: Exception) {
    null
}

/** A missing claim passes; a present one must be a number that satisfies [ok]. */
private inline fun timeOk(claim: JsonElement?, ok: (Double) -> Boolean): Boolean {
    if (claim == null) return true
    val p = claim as? JsonPrimitive ?: return false
    if (p.isString) return false
    val v = p.longOrNull?.toDouble() ?: p.doubleOrNull ?: return false
    return ok(v)
}
