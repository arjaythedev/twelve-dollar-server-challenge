// HS256 JWT verification. Verifies the signature on every request (rule 5):
// nothing about a token is ever cached or remembered.

import Crypto
import Foundation
import Hummingbird

struct AuthUser: Sendable {
    let id: Int64
    let username: String
}

enum JWTError: Error, Sendable {
    /// No Authorization header / not a Bearer token.
    case missingBearer
    /// Bad signature, expired, not HS256, malformed.
    case invalid
    /// Valid JWT whose payload does not satisfy the contract.
    case invalidPayload
}

enum JWT {
    static func verify(_ token: Substring, secret: SymmetricKey) throws -> AuthUser {
        let parts = token.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 3 else { throw JWTError.invalid }

        guard let headerData = base64URLDecode(parts[0]),
            let payloadData = base64URLDecode(parts[1]),
            let signature = base64URLDecode(parts[2])
        else { throw JWTError.invalid }

        // Header must say HS256.
        guard let headerObject = try? JSONSerialization.jsonObject(with: headerData),
            let header = headerObject as? [String: Any],
            let algorithm = header["alg"] as? String,
            algorithm == "HS256"
        else { throw JWTError.invalid }

        // Constant-time signature check over header.payload.
        let signingInput = Data("\(parts[0]).\(parts[1])".utf8)
        guard HMAC<SHA256>.isValidAuthenticationCode(signature, authenticating: signingInput, using: secret)
        else { throw JWTError.invalid }

        guard let payloadObject = try? JSONSerialization.jsonObject(with: payloadData),
            let payload = payloadObject as? [String: Any]
        else { throw JWTError.invalid }

        // exp must be present and in the future.
        guard numberValue(payload["exp"]).map({ $0 > Date().timeIntervalSince1970 }) == true else {
            throw JWTError.invalid
        }

        // sub must be a positive integer string; username must be a string.
        guard let sub = payload["sub"] as? String, let id = Int64(sub), id > 0 else {
            throw JWTError.invalidPayload
        }
        guard let username = payload["username"] as? String else {
            throw JWTError.invalidPayload
        }
        return AuthUser(id: id, username: username)
    }

    static func extract(_ request: Request, secret: SymmetricKey) throws -> AuthUser {
        guard let authorization = request.headers[.authorization], authorization.hasPrefix("Bearer ") else {
            throw JWTError.missingBearer
        }
        return try verify(authorization.dropFirst(7), secret: secret)
    }

    private static func numberValue(_ any: Any?) -> Double? {
        guard let number = any as? NSNumber else { return nil }
        return number.doubleValue
    }

    private static func base64URLDecode(_ segment: Substring) -> Data? {
        var base64 = String(segment)
        if base64.contains("-") { base64 = base64.replacingOccurrences(of: "-", with: "+") }
        if base64.contains("_") { base64 = base64.replacingOccurrences(of: "_", with: "/") }
        switch base64.count % 4 {
        case 2: base64 += "=="
        case 3: base64 += "="
        case 0: break
        default: return nil // invalid length
        }
        return Data(base64Encoded: base64)
    }
}
