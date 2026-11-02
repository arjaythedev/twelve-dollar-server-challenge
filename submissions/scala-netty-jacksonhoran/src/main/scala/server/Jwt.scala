package server

import java.nio.charset.StandardCharsets.UTF_8
import java.security.MessageDigest
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** HS256 JWT verification, done in full on every request: signature, alg, exp, payload shape. */
final class Jwt(secret: String):
  private val mac = Mac.getInstance("HmacSHA256")
  mac.init(SecretKeySpec(secret.getBytes(UTF_8), "HmacSHA256"))
  private val b64 = Base64.getUrlDecoder

  /** Set by a successful verify(). */
  var userId: Long = 0
  var username: String = ""

  /** Verifies the `Authorization` header value `h[off, off+len)`. Returns null when the user is
    * authenticated (userId/username set), otherwise the error response body. */
  def verify(h: Array[Byte], off: Int, len: Int): Array[Byte] =
    if len < 7 || !Jwt.startsWithBearer(h, off) then return Errors.missingToken
    val s = off + 7; val end = off + len
    var d1 = -1; var d2 = -1; var i = s
    while i < end do
      if h(i) == '.' then
        if d1 < 0 then d1 = i else if d2 < 0 then d2 = i else return Errors.invalidToken
      i += 1
    if d2 < 0 then return Errors.invalidToken
    try
      val hb = decode(h, s, d1)
      val header = Json.parse(hb, 0, hb.length) match
        case m: java.util.HashMap[?, ?] => m
        case _ => return Errors.invalidToken
      if header.get("alg") != "HS256" then return Errors.invalidToken

      val sig = decode(h, d2 + 1, end)
      if sig.length != 32 then return Errors.invalidToken
      mac.update(h, s, d2 - s)
      if !MessageDigest.isEqual(mac.doFinal(), sig) then return Errors.invalidToken

      val pb = decode(h, d1 + 1, d2)
      val payload = Json.parse(pb, 0, pb.length) match
        case m: java.util.HashMap[?, ?] => m
        case _ => return Errors.invalidToken
      // exp is required (SPEC.md): a token without one is rejected like an expired one
      payload.get("exp") match
        case exp: java.lang.Double => if exp.doubleValue <= System.currentTimeMillis() / 1000.0 then return Errors.invalidToken
        case _ => return Errors.invalidToken
      payload.get("nbf") match
        case null =>
        case nbf: java.lang.Double => if nbf.doubleValue > System.currentTimeMillis() / 1000.0 then return Errors.invalidToken
        case _ => return Errors.invalidToken

      val id = payload.get("sub") match
        case sub: String => Jwt.positiveInt(sub)
        case _ => -1L
      if id <= 0 then return Errors.invalidPayload
      payload.get("username") match
        case name: String => userId = id; username = name; null
        case _ => Errors.invalidPayload
    catch case _: Json.Malformed | _: IllegalArgumentException => Errors.invalidToken

  /** base64url-decodes h[from, to). Throws IllegalArgumentException on bad input. */
  private def decode(h: Array[Byte], from: Int, to: Int): Array[Byte] =
    b64.decode(java.util.Arrays.copyOfRange(h, from, to))

object Jwt:
  private val bearer = "Bearer ".getBytes(UTF_8)
  private def startsWithBearer(h: Array[Byte], off: Int): Boolean =
    var k = 0
    while k < 7 do { if h(off + k) != bearer(k) then return false; k += 1 }
    true

  /** "123" -> 123; anything that isn't a plain positive decimal integer -> -1. */
  def positiveInt(s: String): Long =
    if s.isEmpty || s.length > 18 then return -1
    var v = 0L; var k = 0
    while k < s.length do
      val c = s.charAt(k)
      if c < '0' || c > '9' then return -1
      v = v * 10 + (c - '0'); k += 1
    if v > 0 then v else -1
