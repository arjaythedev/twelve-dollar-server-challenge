package feed

import io.circe.{Json, parser}

import java.nio.charset.StandardCharsets.{US_ASCII, UTF_8}
import java.security.MessageDigest
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import scala.util.Try

/** The authenticated user, taken from the token as-is (it isn't looked up). */
final case class User(id: Long, username: String)

enum AuthError(val message: String):
  case MissingToken extends AuthError("missing bearer token")
  case InvalidToken extends AuthError("invalid or expired token")
  case InvalidPayload extends AuthError("invalid token payload")

/** HS256 JWT verification. Every request is verified in full: there is no verification cache (rule 5). */
final class Jwt(secret: String):
  import AuthError.*

  private val prototype: Mac =
    val mac = Mac.getInstance("HmacSHA256")
    mac.init(SecretKeySpec(secret.getBytes(UTF_8), "HmacSHA256"))
    mac

  private val decoder = Base64.getUrlDecoder

  /** @param authorization
    *   the `Authorization` header, if any
    * @param nowSeconds
    *   the current time as a Unix timestamp
    */
  def verify(authorization: Option[String], nowSeconds: Long): Either[AuthError, User] =
    for
      token <- authorization.collect { case h if h.startsWith("Bearer ") => h.substring(7) }.toRight(MissingToken)
      claims <- claims(token, nowSeconds).toRight(InvalidToken)
      user <- user(claims).toRight(InvalidPayload)
    yield user

  /** The payload of a well-formed, correctly signed, unexpired HS256 token. */
  private def claims(token: String, nowSeconds: Long): Option[Json] =
    token.split('.') match
      case Array(header, payload, signature) =>
        for
          headerJson <- decodeJson(header)
          if headerJson.hcursor.get[String]("alg").contains("HS256")
          signatureBytes <- decode(signature)
          if MessageDigest.isEqual(sign(s"$header.$payload"), signatureBytes)
          claims <- decodeJson(payload)
          exp <- claims.hcursor.get[Long]("exp").toOption
          if exp > nowSeconds
          if claims.hcursor.get[Long]("nbf").toOption.forall(_ <= nowSeconds)
        yield claims
      case _ => None

  private def user(claims: Json): Option[User] =
    val c = claims.hcursor
    for
      sub <- c.get[String]("sub").toOption
      if sub.nonEmpty && sub.forall(_.isDigit)
      id <- sub.toLongOption
      if id > 0
      username <- c.get[String]("username").toOption
    yield User(id, username)

  private def sign(input: String): Array[Byte] =
    prototype.clone().asInstanceOf[Mac].doFinal(input.getBytes(US_ASCII))

  private def decode(part: String): Option[Array[Byte]] = Try(decoder.decode(part)).toOption

  private def decodeJson(part: String): Option[Json] =
    decode(part).flatMap(bytes => parser.parse(String(bytes, UTF_8)).toOption)
