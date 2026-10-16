// The $12 Server Challenge — Swift + Hummingbird + SQLite.
//
// Config comes from the environment (SPEC.md): SQLITE_PATH, JWT_SECRET, HOST, PORT.
// Optional tuning (all in-process settings, allowed by rule 11):
//   EL_THREADS  event-loop threads           (default 2)
//   READERS     pooled SQLite read connections (default 4)

import Crypto
import Foundation
import Hummingbird
import NIOCore
import NIOPosix

struct AppState: Sendable {
    let database: Database
    // SymmetricKey is immutable after construction; swift-crypto just doesn't
    // annotate it Sendable on Linux.
    nonisolated(unsafe) let jwtKey: SymmetricKey
    let startedAt: Date
}

// MARK: - Handlers

/// Strict positive-integer parse: ASCII digits only, no sign, no dot, > 0.
private func parsePostID(_ raw: String) -> Int64? {
    guard !raw.isEmpty else { return nil }
    var value: Int64 = 0
    for byte in raw.utf8 {
        guard byte >= 48, byte <= 57 else { return nil }
        let digit = Int64(byte - 48)
        guard value <= (Int64.max - digit) / 10 else { return nil }
        value = value &* 10 &+ digit
    }
    return value > 0 ? value : nil
}

private func healthHandler(_ state: AppState) -> Response {
    switch state.database.healthCheck() {
    case .success:
        let uptime = Int(Date().timeIntervalSince(state.startedAt))
        var buffer = ByteBufferAllocator().buffer(capacity: 64)
        buffer.writeString("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
        buffer.writeString(String(uptime))
        buffer.writeString("}")
        return Responses.json(.ok, buffer)
    case .failure(let error):
        var buffer = ByteBufferAllocator().buffer(capacity: 256)
        buffer.writeString("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":")
        writeJSONString(&buffer, error.message)
        buffer.writeString("}")
        return Responses.json(.serviceUnavailable, buffer)
    }
}

private func getPostHandler(_ idParameter: String, _ state: AppState) throws -> Response {
    guard let id = parsePostID(idParameter) else { return Responses.invalidPostID }
    guard let buffer = try state.database.post(id: id) else { return Responses.postNotFound }
    return Responses.json(.ok, buffer)
}

private func createPostHandler(_ request: Request, _ state: AppState) async throws -> Response {
    // Auth first (spec: auth before anything else on authenticated routes).
    let user = try JWT.extract(request, secret: state.jwtKey)

    var request = request
    let body = try await request.collectBody(upTo: 16 * 1024)

    guard let object = try? JSONSerialization.jsonObject(with: Data(body.readableBytesView)),
        let dictionary = object as? [String: Any]
    else { return Responses.malformedJSON }
    guard let rawBody = dictionary["body"] as? String else { return Responses.bodyRequired }

    let trimmed = rawBody.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed.isEmpty { return Responses.bodyRequired }
    if trimmed.unicodeScalars.count > 500 { return Responses.bodyTooLong }

    let (id, createdAt) = try await state.database.createPost(userId: user.id, body: trimmed)

    var buffer = ByteBufferAllocator().buffer(capacity: 1100)
    buffer.writeString("{\"post\":{\"id\":")
    buffer.writeString(String(id))
    buffer.writeString(",\"body\":")
    writeJSONString(&buffer, trimmed)
    buffer.writeString(",\"created_at\":")
    writeJSONString(&buffer, createdAt)
    buffer.writeString(",\"author\":")
    writeJSONString(&buffer, user.username)
    buffer.writeString(",\"like_count\":0}}")
    return Responses.json(.created, buffer)
}

private func likeHandler(_ idParameter: String, _ request: Request, _ state: AppState) async throws -> Response {
    // Auth first, then id validation (spec ordering).
    let user = try JWT.extract(request, secret: state.jwtKey)
    guard let id = parsePostID(idParameter) else { return Responses.invalidPostID }

    let alreadyLiked = try await state.database.like(postId: id, userId: user.id)

    var buffer = ByteBufferAllocator().buffer(capacity: 72)
    buffer.writeString("{\"liked\":true,\"already_liked\":")
    buffer.writeString(alreadyLiked ? "true" : "false")
    buffer.writeString(",\"post_id\":")
    buffer.writeString(String(id))
    buffer.writeString("}")
    return Responses.json(alreadyLiked ? .ok : .created, buffer)
}

// MARK: - Router

private func buildRouter(_ state: AppState) -> Router<BasicRequestContext> {
    let router = Router()
    // Registered before any endpoint so it also wraps the not-found responder.
    router.add(middleware: ErrorMapperMiddleware())

    router.get("health") { _, _ in
        healthHandler(state)
    }
    router.get("feed") { _, _ in
        Responses.json(.ok, try state.database.feed())
    }
    router.get("posts/:id") { _, context in
        try getPostHandler(context.parameters.require("id"), state)
    }
    router.post("posts") { request, _ in
        try await createPostHandler(request, state)
    }
    router.post("posts/:id/like") { request, context in
        try await likeHandler(context.parameters.require("id"), request, state)
    }
    return router
}

// MARK: - Entry point

let environment = ProcessInfo.processInfo.environment

guard let sqlitePath = environment["SQLITE_PATH"] else {
    fatalError("SQLITE_PATH is not set")
}
guard let jwtSecret = environment["JWT_SECRET"] else {
    fatalError("JWT_SECRET is not set")
}
let host = environment["HOST"] ?? "0.0.0.0"
let port = environment["PORT"].flatMap(Int.init) ?? 80
let eventLoopThreads = environment["EL_THREADS"].flatMap(Int.init) ?? 2
let readerCount = max(1, environment["READERS"].flatMap(Int.init) ?? 4)

let state = AppState(
    database: try Database.open(path: sqlitePath, readers: readerCount),
    jwtKey: SymmetricKey(data: Data(jwtSecret.utf8)),
    startedAt: Date()
)

let router = buildRouter(state)

let eventLoopGroup = MultiThreadedEventLoopGroup(numberOfThreads: eventLoopThreads)

let application = Application(
    router: router,
    configuration: ApplicationConfiguration(
        address: .hostname(host, port: port),
        serverName: nil,
        backlog: 4096
    ),
    eventLoopGroupProvider: .shared(eventLoopGroup)
)

try await application.runService()
try? await eventLoopGroup.shutdownGracefully()
