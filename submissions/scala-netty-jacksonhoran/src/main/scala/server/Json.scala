package server

import java.nio.charset.StandardCharsets.UTF_8

/** A strict JSON parser for the small documents this server reads: POST /posts bodies and JWT parts.
  * Objects become java.util.HashMap (last duplicate key wins), strings String, numbers Double. */
object Json:
  object Null
  final class Malformed extends RuntimeException(null, null, false, false)

  def parse(b: Array[Byte], off: Int, len: Int): AnyRef =
    val p = Parser(b, off, off + len)
    p.ws()
    val v = p.value(0)
    p.ws()
    if p.i != p.end then throw Malformed()
    v

  private final class Parser(b: Array[Byte], var i: Int, val end: Int):
    private def fail(): Nothing = throw Malformed()
    private def peek: Int = if i < end then b(i) & 0xff else -1

    def ws(): Unit =
      while i < end && (b(i) == ' ' || b(i) == '\n' || b(i) == '\r' || b(i) == '\t') do i += 1

    def value(depth: Int): AnyRef =
      if depth > 64 then fail()
      peek match
        case '{' => obj(depth)
        case '[' => arr(depth)
        case '"' => str()
        case 't' => lit("true"); java.lang.Boolean.TRUE
        case 'f' => lit("false"); java.lang.Boolean.FALSE
        case 'n' => lit("null"); Null
        case c if c == '-' || (c >= '0' && c <= '9') => num()
        case _ => fail()

    private def lit(s: String): Unit =
      var k = 0
      while k < s.length do
        if i >= end || b(i) != s.charAt(k) then fail()
        i += 1; k += 1

    private def obj(depth: Int): AnyRef =
      val m = java.util.HashMap[String, AnyRef]()
      i += 1; ws()
      if peek == '}' then { i += 1; return m }
      while true do
        if peek != '"' then fail()
        val k = str(); ws()
        if peek != ':' then fail()
        i += 1; ws()
        m.put(k, value(depth + 1)); ws()
        peek match
          case ',' => i += 1; ws()
          case '}' => i += 1; return m
          case _ => fail()
      m

    private def arr(depth: Int): AnyRef =
      val a = java.util.ArrayList[AnyRef]()
      i += 1; ws()
      if peek == ']' then { i += 1; return a }
      while true do
        a.add(value(depth + 1)); ws()
        peek match
          case ',' => i += 1; ws()
          case ']' => i += 1; return a
          case _ => fail()
      a

    private def digits(): Int =
      val s = i
      while i < end && b(i) >= '0' && b(i) <= '9' do i += 1
      i - s

    private def num(): AnyRef =
      val s = i
      if peek == '-' then i += 1
      if peek == '0' then i += 1 else if digits() == 0 then fail()
      if peek == '.' then { i += 1; if digits() == 0 then fail() }
      if peek == 'e' || peek == 'E' then
        i += 1
        if peek == '+' || peek == '-' then i += 1
        if digits() == 0 then fail()
      java.lang.Double.valueOf(String(b, s, i - s, UTF_8))

    def str(): String =
      i += 1
      val start = i
      // fast path: no escapes
      while i < end && b(i) != '"' && b(i) != '\\' && (b(i) & 0xff) >= 0x20 do i += 1
      if i < end && b(i) == '"' then { i += 1; return String(b, start, i - 1 - start, UTF_8) }
      val sb = java.lang.StringBuilder()
      sb.append(String(b, start, i - start, UTF_8))
      while true do
        if i >= end then fail()
        val c = b(i) & 0xff
        if c == '"' then { i += 1; return sb.toString }
        else if c < 0x20 then fail()
        else if c == '\\' then
          if i + 1 >= end then fail()
          b(i + 1) match
            case '"' => sb.append('"'); i += 2
            case '\\' => sb.append('\\'); i += 2
            case '/' => sb.append('/'); i += 2
            case 'b' => sb.append('\b'); i += 2
            case 'f' => sb.append('\f'); i += 2
            case 'n' => sb.append('\n'); i += 2
            case 'r' => sb.append('\r'); i += 2
            case 't' => sb.append('\t'); i += 2
            case 'u' =>
              if i + 6 > end then fail()
              var v = 0; var k = i + 2
              while k < i + 6 do
                val h = Character.digit(b(k).toInt, 16)
                if h < 0 then fail()
                v = v * 16 + h; k += 1
              sb.append(v.toChar); i += 6
            case _ => fail()
        else
          val s = i
          while i < end && b(i) != '"' && b(i) != '\\' && (b(i) & 0xff) >= 0x20 do i += 1
          sb.append(String(b, s, i - s, UTF_8))
      sb.toString
