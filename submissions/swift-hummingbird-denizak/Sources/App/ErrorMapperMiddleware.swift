// Catches anything thrown out of the router (unknown paths, framework errors,
// bugs) and turns it into the exact error bodies required by SPEC.md.

import Hummingbird

struct ErrorMapperMiddleware<Context: RequestContext>: RouterMiddleware {
    func handle(
        _ request: Request,
        context: Context,
        next: (Request, Context) async throws -> Response
    ) async throws -> Response {
        do {
            return try await next(request, context)
        } catch let error as JWTError {
            switch error {
            case .missingBearer: return Responses.missingBearer
            case .invalid: return Responses.invalidToken
            case .invalidPayload: return Responses.invalidPayload
            }
        } catch is LogicError {
            return Responses.postNotFound
        } catch let error as HTTPResponseError {
            if error.status == .notFound { return Responses.notFound }
            return Response(
                status: error.status,
                headers: [.contentType: "application/json"],
                body: .init(byteBuffer: Responses.internalErrorBuffer)
            )
        } catch {
            return Responses.internalError
        }
    }
}
