import { createHmac, createSecretKey, timingSafeEqual } from "node:crypto"
import { Config, Context, DateTime, Effect, Layer, Redacted, Schema, SchemaTransformation } from "effect"
import { HttpServerRespondable } from "effect/http"
import { errorResponse } from "../Http/errorResponse.ts"

const missingBearerToken = errorResponse(401, "missing bearer token")

export class MissingBearerToken extends Schema.TaggedError<MissingBearerToken>()("MissingBearerToken", {}) {
  [HttpServerRespondable.symbol]() {
    return missingBearerToken
  }
}

const invalidToken = errorResponse(401, "invalid or expired token")

export class InvalidToken extends Schema.TaggedError<InvalidToken>()("InvalidToken", {
  reason: Schema.Literals(["Malformed", "Signature", "Expired"])
}) {
  [HttpServerRespondable.symbol]() {
    return invalidToken
  }
}

const invalidTokenPayload = errorResponse(401, "invalid token payload")

export class InvalidTokenPayload extends Schema.TaggedError<InvalidTokenPayload>()("InvalidTokenPayload", {}) {
  [HttpServerRespondable.symbol]() {
    return invalidTokenPayload
  }
}

const BearerToken = Schema.TemplateLiteralParser(["Bearer ", Schema.String])

const TokenParts = Schema.Tuple([Schema.String, Schema.String, Schema.String])

const segment = <S extends Schema.Top>(schema: S) =>
  Schema.StringFromBase64Url.pipe(
    Schema.decodeTo(Schema.fromJsonString(schema))
  )

const TokenHeader = segment(Schema.Struct({
  alg: Schema.Literal("HS256")
}))

const TokenSignature = Schema.Uint8ArrayFromBase64Url

const ExpiresAt = Schema.Finite.pipe(
  Schema.decodeTo(
    Schema.DateTimeUtcFromMillis,
    SchemaTransformation.transform({
      decode: (seconds: number) => seconds * 1000,
      encode: (millis: number) => millis / 1000
    })
  )
)

const TokenClaims = segment(Schema.Struct({
  exp: ExpiresAt,
  sub: Schema.optionalKey(Schema.Unknown),
  username: Schema.optionalKey(Schema.Unknown)
}))

const UserIdFromString = Schema.String.check(Schema.isPattern(/^[1-9][0-9]*$/)).pipe(
  Schema.decodeTo(Schema.Int, SchemaTransformation.numberFromString)
)

const Identity = Schema.Struct({
  sub: UserIdFromString,
  username: Schema.String
})

const decodeBearerToken = Schema.decodeUnknownEffect(BearerToken)
const decodeTokenParts = Schema.decodeUnknownEffect(TokenParts)
const decodeHeader = Schema.decodeUnknownEffect(TokenHeader)
const decodeSignature = Schema.decodeUnknownEffect(TokenSignature)
const decodeClaims = Schema.decodeUnknownEffect(TokenClaims)
const decodeIdentity = Schema.decodeUnknownEffect(Identity)

const signaturesMatch = (expected: Uint8Array, actual: Uint8Array) => {
  if (expected.length !== actual.length) return false

  return timingSafeEqual(expected, actual)
}

const malformed = () => new InvalidToken({ reason: "Malformed" })

export class Auth extends Context.Service<Auth, {
  authenticate(authorization: string | undefined): Effect.Effect<
    { readonly userId: number; readonly username: string },
    MissingBearerToken | InvalidToken | InvalidTokenPayload
  >
}>()("tdsc/Auth/Auth") {
  static readonly layer = Layer.effect(
    Auth,
    Effect.gen(function*() {
      const secret = yield* Config.Redacted("JWT_SECRET")
      const key = createSecretKey(Buffer.from(Redacted.value(secret)))

      const sign = (signingInput: string) => createHmac("sha256", key).update(signingInput).digest()

      const authenticate = Effect.fn("Auth.authenticate")(function*(authorization: string | undefined) {
        const [, token] = yield* decodeBearerToken(authorization).pipe(
          Effect.mapError(() => new MissingBearerToken())
        )

        const [header, payload, signature] = yield* decodeTokenParts(token.split(".")).pipe(
          Effect.mapError(malformed)
        )

        yield* decodeHeader(header).pipe(Effect.mapError(malformed))

        const actual = yield* decodeSignature(signature).pipe(Effect.mapError(malformed))

        if (!signaturesMatch(sign(`${header}.${payload}`), actual)) {
          return yield* new InvalidToken({ reason: "Signature" })
        }

        const claims = yield* decodeClaims(payload).pipe(Effect.mapError(malformed))
        const expired = yield* DateTime.isPast(claims.exp)

        if (expired) return yield* new InvalidToken({ reason: "Expired" })

        const identity = yield* decodeIdentity(claims).pipe(
          Effect.mapError(() => new InvalidTokenPayload())
        )

        return { userId: identity.sub, username: identity.username }
      })

      return Auth.of({ authenticate })
    })
  )
}
