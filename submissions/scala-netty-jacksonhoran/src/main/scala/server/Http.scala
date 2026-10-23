package server

import io.netty.buffer.{ByteBuf, Unpooled}
import io.netty.channel.{Channel, ChannelFutureListener, ChannelHandlerContext, ChannelInboundHandlerAdapter}
import java.nio.charset.StandardCharsets.{US_ASCII, UTF_8}
import java.time.{ZoneOffset, ZonedDateTime}
import java.time.format.DateTimeFormatter
import java.util.Locale

/** A small HTTP/1.1 server over raw Netty bytes: request line, the few headers we need, Content-Length
  * or chunked bodies, keep-alive and pipelining. Everything runs on the single event-loop thread, so
  * the parse state below is shared scratch space rather than per-connection objects: an idle connection
  * costs just its Netty channel and one small handler. */
object Http:
  private final val MaxHead = 16 * 1024
  private final val MaxBody = 1024 * 1024

  private var in = new Array[Byte](64 * 1024) // the bytes being parsed (leftover + new read)
  private var chunked = new Array[Byte](16 * 1024) // a decoded chunked body

  // parse results for the current request
  private var method = 0
  private var tOff = 0; private var tLen = 0
  private var authOff = 0; private var authLen = -1
  private var contentLength = 0L
  private var isChunked = false
  private var close = false
  private var expectContinue = false
  private var bodyOff = 0; private var bodyLen = 0; private var bodyArr: Array[Byte] = null

  @volatile private var date: Array[Byte] = dateHeader()
  def dateHeader(): Array[Byte] =
    ("\r\nDate: " + DateTimeFormatter.ofPattern("EEE, dd MMM yyyy HH:mm:ss 'GMT'", Locale.US)
      .format(ZonedDateTime.now(ZoneOffset.UTC))).getBytes(US_ASCII)
  def tickDate(): Unit = date = dateHeader()

  private val statusLines: Map[Int, Array[Byte]] = Map(
    200 -> "OK", 201 -> "Created", 400 -> "Bad Request", 401 -> "Unauthorized", 404 -> "Not Found",
    413 -> "Content Too Large", 431 -> "Request Header Fields Too Large", 500 -> "Internal Server Error",
    503 -> "Service Unavailable",
  ).map((k, v) => k -> s"HTTP/1.1 $k $v\r\nContent-Type: application/json".getBytes(US_ASCII))
  private val contentLengthHeader = "\r\nContent-Length: ".getBytes(US_ASCII)
  private val connectionClose = "\r\nConnection: close".getBytes(US_ASCII)
  private val continue100 = "HTTP/1.1 100 Continue\r\n\r\n".getBytes(US_ASCII)
  private val badRequest = """{"error":"bad request"}""".getBytes(UTF_8)
  private val tooLarge = """{"error":"request too large"}""".getBytes(UTF_8)

  /** Open connections in least-recently-active order, for reaping idle ones under fd pressure.
    * Normally nothing is ever reaped: it only starts when open connections near the process's
    * file-descriptor limit, where the alternative is failing to accept new connections at all. */
  private object Conns:
    private var head: Handler = null // least recently active
    private var tail: Handler = null
    var open = 0
    var reaped = 0L
    private val fdLimit: Long = java.lang.management.ManagementFactory.getOperatingSystemMXBean match
      case os: com.sun.management.UnixOperatingSystemMXBean => os.getMaxFileDescriptorCount
      case _ => -1L
    /** Reap above `high` open connections (fds kept back for the JVM, SQLite and the listener) down to `low`. */
    val high: Int = if fdLimit <= 0 then Int.MaxValue else Math.max(fdLimit - 1024, fdLimit * 9 / 10).toInt
    val low: Int = if high == Int.MaxValue then high else high - Math.max(high / 50, 16)
    /** Never close a connection that was active within this window, to keep clear of requests in flight. */
    private final val MinIdleNanos = 1_000_000_000L

    def add(h: Handler): Unit =
      h.prev = tail; h.next = null
      if tail == null then head = h else tail.next = h
      tail = h
      open += 1
      if open > high then reap()

    def remove(h: Handler): Unit =
      if h.prev == null then head = h.next else h.prev.next = h.next
      if h.next == null then tail = h.prev else h.next.prev = h.prev
      h.prev = null; h.next = null
      open -= 1

    def touch(h: Handler): Unit =
      h.lastActive = System.nanoTime()
      if h ne tail then { remove(h); add(h) }

    private def reap(): Unit =
      val now = System.nanoTime()
      val before = reaped
      while open > low && head != null && now - head.lastActive > MinIdleNanos do
        val h = head
        remove(h)
        h.reaped = true
        reaped += 1
        h.channel.close()
      if before == 0 && reaped > 0 then
        System.err.println(s"$open connections, near the fd limit ($fdLimit): closing the longest-idle ones")

  def reapThresholds: String =
    if Conns.high == Int.MaxValue then "fd limit unknown, idle connections are never reaped"
    else s"idle connections are reaped above ${Conns.high} open, down to ${Conns.low}"

  final class Handler(app: App) extends ChannelInboundHandlerAdapter:
    private var pending: Array[Byte] = null // an incomplete request left over from earlier reads
    private var sentContinue = false
    private var wrote = false
    // Conns bookkeeping
    private[Http] var channel: Channel = null
    private[Http] var prev: Handler = null
    private[Http] var next: Handler = null
    private[Http] var lastActive = 0L
    private[Http] var reaped = false

    override def channelActive(ctx: ChannelHandlerContext): Unit =
      channel = ctx.channel()
      lastActive = System.nanoTime()
      Conns.add(this)
      ctx.fireChannelActive()

    override def channelRead(ctx: ChannelHandlerContext, msg: Any): Unit =
      Conns.touch(this)
      val buf = msg.asInstanceOf[ByteBuf]
      val p = if pending == null then 0 else pending.length
      val total = p + buf.readableBytes
      if in.length < total then in = java.util.Arrays.copyOf(in, Math.max(total, in.length * 2))
      if p > 0 then System.arraycopy(pending, 0, in, 0, p)
      buf.readBytes(in, p, buf.readableBytes)
      buf.release()
      pending = null

      var pos = 0
      var open = true
      var more = true
      while open && more && pos < total do
        val used = request(ctx, pos, total)
        if used > 0 then pos += used
        else if used == 0 then more = false
        else open = false
      if open && pos < total then
        if total - pos > MaxHead + MaxBody then reply(ctx, 413, tooLarge, true)
        else pending = java.util.Arrays.copyOfRange(in, pos, total)

    override def channelReadComplete(ctx: ChannelHandlerContext): Unit =
      if wrote then { wrote = false; ctx.flush() }

    override def channelInactive(ctx: ChannelHandlerContext): Unit =
      pending = null
      if channel != null && !reaped then Conns.remove(this)
      channel = null
      ctx.fireChannelInactive()

    override def exceptionCaught(ctx: ChannelHandlerContext, cause: Throwable): Unit = ctx.close()

    /** Handles one request at in[pos, end). Returns the bytes it used, 0 if the request is not
      * complete yet, or -1 if the connection was closed. */
    private def request(ctx: ChannelHandlerContext, pos: Int, end: Int): Int =
      val headEnd = findHeadEnd(pos, end)
      if headEnd < 0 then
        if end - pos > MaxHead then { reply(ctx, 431, tooLarge, true); return -1 }
        return 0
      if !parseHead(pos, headEnd) then { reply(ctx, 400, badRequest, true); return -1 }

      var reqEnd = 0
      if isChunked then
        reqEnd = dechunk(headEnd, end)
        if reqEnd == -2 then { reply(ctx, 413, tooLarge, true); return -1 }
        if reqEnd == -1 then { reply(ctx, 400, badRequest, true); return -1 }
        bodyArr = chunked; bodyOff = 0
      else
        if contentLength > MaxBody then { reply(ctx, 413, tooLarge, true); return -1 }
        bodyArr = in; bodyOff = headEnd; bodyLen = contentLength.toInt
        reqEnd = if end - headEnd >= bodyLen then headEnd + bodyLen else 0
      if reqEnd == 0 then
        if expectContinue && !sentContinue then
          sentContinue = true
          ctx.writeAndFlush(Unpooled.wrappedBuffer(continue100), ctx.voidPromise())
        return 0
      sentContinue = false

      val status = app.handle(method, in, tOff, tLen, authOff, authLen, bodyArr, bodyOff, bodyLen)
      val out = app.out
      writeResponse(ctx, status, out.buf, out.len, close)
      if close then { closeAfterFlush(ctx); -1 } else reqEnd - pos

    private def reply(ctx: ChannelHandlerContext, status: Int, body: Array[Byte], closeIt: Boolean): Unit =
      writeResponse(ctx, status, body, body.length, closeIt)
      if closeIt then closeAfterFlush(ctx)

    private def closeAfterFlush(ctx: ChannelHandlerContext): Unit =
      wrote = false
      pending = null
      ctx.writeAndFlush(Unpooled.EMPTY_BUFFER).addListener(ChannelFutureListener.CLOSE)

    private def writeResponse(ctx: ChannelHandlerContext, status: Int, body: Array[Byte], len: Int, closeIt: Boolean): Unit =
      val line = statusLines(status)
      val d = date
      val b = ctx.alloc().ioBuffer(line.length + d.length + 64 + len)
      b.writeBytes(line).writeBytes(d).writeBytes(contentLengthHeader)
      writeDecimal(b, len)
      if closeIt then b.writeBytes(connectionClose)
      b.writeShort(0x0d0a).writeShort(0x0d0a)
      b.writeBytes(body, 0, len)
      ctx.write(b, ctx.voidPromise())
      wrote = true

  private def writeDecimal(b: ByteBuf, v: Int): Unit =
    if v >= 10 then writeDecimal(b, v / 10)
    b.writeByte('0' + v % 10)

  /** Index just past the blank line ending the head, or -1. */
  private def findHeadEnd(pos: Int, end: Int): Int =
    var i = pos + 3
    while i < end do
      if in(i) == '\n' && in(i - 1) == '\r' && in(i - 2) == '\n' && in(i - 3) == '\r' then return i + 1
      i += 1
    -1

  private def parseHead(pos: Int, headEnd: Int): Boolean =
    authLen = -1; contentLength = 0; isChunked = false; expectContinue = false
    // request line: METHOD SP target SP HTTP/1.x CRLF
    var i = pos
    while i < headEnd && in(i) != ' ' do i += 1
    method =
      if i - pos == 3 && in(pos) == 'G' && in(pos + 1) == 'E' && in(pos + 2) == 'T' then App.GET
      else if i - pos == 4 && in(pos) == 'P' && in(pos + 1) == 'O' && in(pos + 2) == 'S' && in(pos + 3) == 'T' then App.POST
      else 0
    i += 1; tOff = i
    while i < headEnd && in(i) != ' ' && in(i) != '\r' do i += 1
    tLen = i - tOff
    if tLen == 0 || i >= headEnd || in(i) != ' ' then return false
    i += 1
    val v = i
    while i < headEnd && in(i) != '\r' do i += 1
    if i - v != 8 || !lit(v, "HTTP/1.") then return false
    val http10 = in(v + 7) == '0'
    if !http10 && in(v + 7) != '1' then return false
    var keepAlive = false
    close = false
    i += 2

    // headers
    while i < headEnd - 2 do
      val ls = i
      while in(i) != '\r' do i += 1
      val le = i
      i += 2
      var c = ls
      while c < le && in(c) != ':' do c += 1
      if c == le || c == ls then return false
      var vs = c + 1
      while vs < le && (in(vs) == ' ' || in(vs) == '\t') do vs += 1
      var ve = le
      while ve > vs && (in(ve - 1) == ' ' || in(ve - 1) == '\t') do ve -= 1
      c - ls match
        case 14 if name(ls, "content-length") =>
          if ve == vs || ve - vs > 18 then return false
          var n = 0L; var k = vs
          while k < ve do
            if in(k) < '0' || in(k) > '9' then return false
            n = n * 10 + (in(k) - '0'); k += 1
          contentLength = n
        case 17 if name(ls, "transfer-encoding") => isChunked = contains(vs, ve, "chunked")
        case 10 if name(ls, "connection") =>
          if contains(vs, ve, "close") then close = true
          if contains(vs, ve, "keep-alive") then keepAlive = true
        case 13 if name(ls, "authorization") => authOff = vs; authLen = ve - vs
        case 6 if name(ls, "expect") => expectContinue = contains(vs, ve, "100-continue")
        case _ =>
    if http10 && !keepAlive then close = true
    true

  private def lit(at: Int, s: String): Boolean =
    var k = 0
    while k < s.length do { if in(at + k) != s.charAt(k) then return false; k += 1 }
    true

  /** Case-insensitive header-name match against a lowercase name. */
  private def name(at: Int, s: String): Boolean =
    var k = 0
    while k < s.length do { if (in(at + k) | 0x20) != s.charAt(k) && in(at + k) != s.charAt(k) then return false; k += 1 }
    true

  /** Case-insensitive search for a lowercase token in in[s, e). */
  private def contains(s: Int, e: Int, tok: String): Boolean =
    var i = s
    while i + tok.length <= e do
      var k = 0
      while k < tok.length && (in(i + k) | 0x20) == (tok.charAt(k) | 0x20) do k += 1
      if k == tok.length then return true
      i += 1
    false

  /** Decodes a chunked body that starts at `from` into `chunked`/bodyLen. Returns the index just past
    * it, 0 if it is incomplete, -1 if malformed, -2 if too large. */
  private def dechunk(from: Int, end: Int): Int =
    var i = from
    bodyLen = 0
    while true do
      val ls = i
      while i + 1 < end && !(in(i) == '\r' && in(i + 1) == '\n') do i += 1
      if i + 1 >= end then return if i - ls > 1024 then -1 else 0
      var size = 0L; var k = ls
      while k < i && in(k) != ';' do
        val h = Character.digit(in(k).toInt, 16)
        if h < 0 || size > MaxBody then return -1
        size = size * 16 + h; k += 1
      if k == ls then return -1
      i += 2
      if size == 0 then
        // trailers, then a blank line
        while true do
          val ts = i
          while i + 1 < end && !(in(i) == '\r' && in(i + 1) == '\n') do i += 1
          if i + 1 >= end then return 0
          i += 2
          if i - 2 == ts then return i
      if bodyLen + size > MaxBody then return -2
      if end - i < size + 2 then return 0
      if chunked.length < bodyLen + size then
        chunked = java.util.Arrays.copyOf(chunked, Math.max(bodyLen + size.toInt, chunked.length * 2))
      System.arraycopy(in, i, chunked, bodyLen, size.toInt)
      bodyLen += size.toInt
      i += size.toInt
      if in(i) != '\r' || in(i + 1) != '\n' then return -1
      i += 2
    -1
