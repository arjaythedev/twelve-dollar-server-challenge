// Exact error bodies from SPEC.md, built once. Response / ByteBuffer are value
// types with copy-on-write storage, so sharing the same instance across requests
// is safe and allocation-free.

import HTTPTypes
import HummingbirdCore
import NIOCore

enum Responses {
    static let notFound = make(.notFound, #"{"error":"not found"}"#)
    static let invalidPostID = make(.badRequest, #"{"error":"invalid post id"}"#)
    static let postNotFound = make(.notFound, #"{"error":"post not found"}"#)
    static let missingBearer = make(.unauthorized, #"{"error":"missing bearer token"}"#)
    static let invalidToken = make(.unauthorized, #"{"error":"invalid or expired token"}"#)
    static let invalidPayload = make(.unauthorized, #"{"error":"invalid token payload"}"#)
    static let malformedJSON = make(.badRequest, #"{"error":"malformed JSON body"}"#)
    static let bodyRequired = make(.badRequest, #"{"error":"body is required"}"#)
    static let bodyTooLong = make(.badRequest, #"{"error":"body must be at most 500 characters"}"#)
    static let internalError = make(.internalServerError, #"{"error":"internal server error"}"#)
    static let internalErrorBuffer: ByteBuffer = {
        var buffer = ByteBufferAllocator().buffer(capacity: 40)
        buffer.writeStaticString(#"{"error":"internal server error"}"#)
        return buffer
    }()

    static func make(_ status: HTTPResponse.Status, _ body: StaticString) -> Response {
        var buffer = ByteBufferAllocator().buffer(capacity: body.utf8CodeUnitCount)
        buffer.writeStaticString(body)
        return Response(status: status, headers: [.contentType: "application/json"], body: .init(byteBuffer: buffer))
    }

    static func json(_ status: HTTPResponse.Status, _ body: ByteBuffer) -> Response {
        Response(status: status, headers: [.contentType: "application/json"], body: .init(byteBuffer: body))
    }
}
