package server

import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.nio.charset.StandardCharsets.UTF_8

/** A growable byte buffer for building one response body. There is one per thread and it is reused. */
final class Out(initial: Int):
  var buf: Array[Byte] = new Array[Byte](initial)
  var len: Int = 0

  def clear(): Unit = len = 0

  private def ensure(n: Int): Unit =
    if len + n > buf.length then buf = java.util.Arrays.copyOf(buf, Math.max(len + n, buf.length * 2))

  def byte(b: Int): Unit =
    ensure(1); buf(len) = b.toByte; len += 1

  def bytes(b: Array[Byte]): Unit =
    ensure(b.length); System.arraycopy(b, 0, buf, len, b.length); len += b.length

  def long(v: Long): Unit =
    if v < 0 then
      if v == Long.MinValue then { bytes("-9223372036854775808".getBytes(UTF_8)); return }
      byte('-'); long(-v)
    else
      ensure(20)
      var digits = 1; var t = v / 10
      while t != 0 do { digits += 1; t /= 10 }
      var i = len + digits - 1; var x = v
      while i >= len do { buf(i) = ('0' + (x % 10)).toByte; x /= 10; i -= 1 }
      len += digits

  /** Raw UTF-8 bytes from SQLite, written as a JSON string. Non-ASCII passes through unescaped. */
  def jsonUtf8(src: MemorySegment, n: Int): Unit =
    ensure(n + 2)
    buf(len) = '"'
    MemorySegment.copy(src, JAVA_BYTE, 0, buf, len + 1, n)
    var i = len + 1; val end = len + 1 + n
    while i < end && Out.plain(buf(i)) do i += 1
    if i == end then { buf(end) = '"'; len = end + 1 }
    else
      // rare: something needs escaping. Move the raw bytes out of the way and escape them.
      val raw = java.util.Arrays.copyOfRange(buf, len + 1, end)
      len += 1
      escape(raw, 0, n)
      byte('"')

  def jsonString(s: String): Unit =
    val b = s.getBytes(UTF_8)
    byte('"'); escape(b, 0, b.length); byte('"')

  private def escape(b: Array[Byte], off: Int, n: Int): Unit =
    var i = off; val end = off + n
    while i < end do
      val c = b(i)
      if Out.plain(c) then byte(c)
      else c.toInt match
        case 0x22 => byte('\\'); byte('"')
        case 0x5c => byte('\\'); byte('\\')
        case 0x0a => byte('\\'); byte('n')
        case 0x0d => byte('\\'); byte('r')
        case 0x09 => byte('\\'); byte('t')
        case _ =>
          byte('\\'); byte('u'); byte('0'); byte('0')
          byte(Out.hex((c >> 4) & 0xf)); byte(Out.hex(c & 0xf))
      i += 1

object Out:
  private val hex = "0123456789abcdef".getBytes(UTF_8)
  /** Bytes that go into a JSON string as-is: everything but control characters, quote and backslash. */
  inline def plain(c: Byte): Boolean = (c >= 0x20 || c < 0) && c != '"' && c != '\\'
