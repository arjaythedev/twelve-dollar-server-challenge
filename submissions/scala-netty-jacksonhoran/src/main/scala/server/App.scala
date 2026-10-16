package server

import java.nio.charset.StandardCharsets.UTF_8

object Errors:
  private def e(msg: String) = s"""{"error":"$msg"}""".getBytes(UTF_8)
  val notFound = e("not found")
  val invalidId = e("invalid post id")
  val postNotFound = e("post not found")
  val missingToken = e("missing bearer token")
  val invalidToken = e("invalid or expired token")
  val invalidPayload = e("invalid token payload")
  val malformedJson = e("malformed JSON body")
  val bodyRequired = e("body is required")
  val bodyTooLong = e("body must be at most 500 characters")
  val internal = e("internal server error")

/** The five endpoints. Every response body is built into `out`; handle() returns the status code.
  * Every request runs its queries against SQLite while it is served: nothing is cached. */
final class App(db: Sqlite.Db, jwt: Jwt):
  import App.*

  val out = Out(16 * 1024)
  private val started = System.nanoTime()

  private val ping = db.prepare("SELECT 1")
  private val feed = db.prepare(PostColumns + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
  private val onePost = db.prepare(PostColumns + " WHERE p.id = ?1")
  private val insertPost = db.prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at")
  // One statement inserts the like only if the post exists; a second query runs only when it inserted
  // nothing, to tell "already liked" from "no such post".
  private val insertLike = db.prepare(
    "INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) " +
      "ON CONFLICT (user_id, post_id) DO NOTHING")
  private val postExists = db.prepare("SELECT 1 FROM posts WHERE id = ?1")

  def handle(method: Int, r: Array[Byte], tOff: Int, tLen: Int, authOff: Int, authLen: Int,
             body: Array[Byte], bOff: Int, bLen: Int): Int =
    out.clear()
    // the path, without a query string
    var pLen = 0
    while pLen < tLen && r(tOff + pLen) != '?' do pLen += 1
    try
      if is(r, tOff, pLen, FeedPath) then
        if method == GET then getFeed() else error(404, Errors.notFound)
      else if is(r, tOff, pLen, PostsPath) then
        if method == POST then createPost(r, authOff, authLen, body, bOff, bLen) else error(404, Errors.notFound)
      else if pLen > PostsSlash.length && startsWith(r, tOff, PostsSlash) then
        val s = tOff + PostsSlash.length; val e = tOff + pLen
        var slash = s
        while slash < e && r(slash) != '/' do slash += 1
        if slash == e then
          if method == GET then getPost(postId(r, s, e)) else error(404, Errors.notFound)
        else if method == POST && is(r, slash, e - slash, LikeSuffix) then
          like(r, authOff, authLen, r, s, slash)
        else error(404, Errors.notFound)
      else if is(r, tOff, pLen, HealthPath) then
        if method == GET then health() else error(404, Errors.notFound)
      else error(404, Errors.notFound)
    catch case _: Sqlite.SqliteException => error(500, Errors.internal)

  private def error(status: Int, body: Array[Byte]): Int =
    out.clear(); out.bytes(body); status

  private def health(): Int =
    try
      ping.step(); ping.reset()
      out.bytes(HealthOk); out.long((System.nanoTime() - started) / 1_000_000_000L); out.byte('}')
      200
    catch case e: Sqlite.SqliteException =>
      out.clear(); out.bytes(HealthDegraded); out.jsonString(String.valueOf(e.getMessage)); out.byte('}')
      503

  private def getFeed(): Int =
    out.bytes(FeedOpen)
    try
      var first = true
      while feed.step() == Sqlite.ROW do
        if !first then out.byte(',')
        first = false
        writePost(feed)
    finally feed.reset()
    out.byte(']'); out.byte('}')
    200

  private def getPost(id: Long): Int =
    if id <= 0 then return error(400, Errors.invalidId)
    onePost.bind(1, id)
    try
      if onePost.step() != Sqlite.ROW then return error(404, Errors.postNotFound)
      out.bytes(PostOpen); writePost(onePost); out.byte('}')
      200
    finally onePost.reset()

  /** {"id":..,"body":..,"created_at":..,"author":..,"like_count":..} from the current row of PostColumns */
  private def writePost(s: Sqlite.Stmt): Unit =
    out.bytes(IdKey); out.long(s.long(0))
    out.bytes(BodyKey); s.jsonText(1, out)
    out.bytes(CreatedKey); s.jsonText(2, out)
    out.bytes(AuthorKey); s.jsonText(3, out)
    out.bytes(LikesKey); out.long(s.long(4))
    out.byte('}')

  private def authError(r: Array[Byte], authOff: Int, authLen: Int): Array[Byte] =
    if authLen < 0 then Errors.missingToken else jwt.verify(r, authOff, authLen)

  private def createPost(r: Array[Byte], authOff: Int, authLen: Int, b: Array[Byte], bOff: Int, bLen: Int): Int =
    val err = authError(r, authOff, authLen)
    if err != null then return error(401, err)
    val doc =
      try Json.parse(b, bOff, bLen)
      catch case _: Json.Malformed => return error(400, Errors.malformedJson)
    val text = doc match
      case m: java.util.HashMap[?, ?] => m.get("body") match
        case s: String => trim(s)
        case _ => return error(400, Errors.bodyRequired)
      case _ => return error(400, Errors.bodyRequired)
    if text.isEmpty then return error(400, Errors.bodyRequired)
    if text.codePointCount(0, text.length) > 500 then return error(400, Errors.bodyTooLong)

    val utf8 = text.getBytes(UTF_8)
    insertPost.bind(1, jwt.userId)
    insertPost.bind(2, utf8, utf8.length)
    try
      if insertPost.step() != Sqlite.ROW then return error(500, Errors.internal)
      out.bytes(PostOpen)
      out.bytes(IdKey); out.long(insertPost.long(0))
      out.bytes(BodyKey); out.jsonString(text)
      out.bytes(CreatedKey); insertPost.jsonText(1, out)
      out.bytes(AuthorKey); out.jsonString(jwt.username)
      out.bytes(LikesKey); out.byte('0'); out.byte('}'); out.byte('}')
      // RETURNING hands back the row before the statement finishes: step to DONE so it commits
      // before we respond.
      while insertPost.step() == Sqlite.ROW do ()
    finally insertPost.reset()
    201

  private def like(r: Array[Byte], authOff: Int, authLen: Int, p: Array[Byte], s: Int, e: Int): Int =
    val err = authError(r, authOff, authLen)
    if err != null then return error(401, err)
    val id = postId(p, s, e)
    if id <= 0 then return error(400, Errors.invalidId)
    insertLike.bind(1, jwt.userId); insertLike.bind(2, id)
    try insertLike.step()
    finally insertLike.reset()
    val status =
      if db.changeCount == 1 then 201
      else
        postExists.bind(1, id)
        val exists = try postExists.step() == Sqlite.ROW finally postExists.reset()
        if !exists then return error(404, Errors.postNotFound)
        200
    out.bytes(if status == 201 then LikedNew else LikedAgain); out.long(id); out.byte('}')
    status

object App:
  final val GET = 1
  final val POST = 2

  private val PostColumns =
    "SELECT p.id, p.body, p.created_at, u.username, " +
      "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id"

  private def b(s: String) = s.getBytes(UTF_8)
  private val FeedPath = b("/feed")
  private val PostsPath = b("/posts")
  private val PostsSlash = b("/posts/")
  private val LikeSuffix = b("/like")
  private val HealthPath = b("/health")
  private val HealthOk = b("""{"status":"ok","db":"ok","uptime_s":""")
  private val HealthDegraded = b("""{"status":"degraded","db":"unreachable","error":""")
  private val FeedOpen = b("""{"posts":[""")
  private val PostOpen = b("""{"post":""")
  private val IdKey = b("""{"id":""")
  private val BodyKey = b(""","body":""")
  private val CreatedKey = b(""","created_at":""")
  private val AuthorKey = b(""","author":""")
  private val LikesKey = b(""","like_count":""")
  private val LikedNew = b("""{"liked":true,"already_liked":false,"post_id":""")
  private val LikedAgain = b("""{"liked":true,"already_liked":true,"post_id":""")

  private def is(r: Array[Byte], off: Int, len: Int, lit: Array[Byte]): Boolean =
    len == lit.length && java.util.Arrays.equals(r, off, off + len, lit, 0, len)
  private def startsWith(r: Array[Byte], off: Int, lit: Array[Byte]): Boolean =
    java.util.Arrays.equals(r, off, off + lit.length, lit, 0, lit.length)

  /** A post id segment: a positive decimal integer, else -1 (ids past 18 digits are out of scope). */
  private def postId(r: Array[Byte], s: Int, e: Int): Long =
    if e <= s || e - s > 18 then return -1
    var v = 0L; var i = s
    while i < e do
      val c = r(i)
      if c < '0' || c > '9' then return -1
      v = v * 10 + (c - '0'); i += 1
    if v > 0 then v else -1

  /** Strips leading and trailing whitespace, the same characters JavaScript's trim() removes. */
  private def trim(s: String): String =
    var a = 0; var z = s.length
    while a < z && space(s.charAt(a)) do a += 1
    while z > a && space(s.charAt(z - 1)) do z -= 1
    s.substring(a, z)
  private def space(c: Char): Boolean = Character.isWhitespace(c) || Character.isSpaceChar(c) || c == '﻿'
