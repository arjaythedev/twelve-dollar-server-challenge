# frozen_string_literal: true
require "sqlite3"
require "json"
require "openssl"

class FeedApp
  HEADERS = { "content-type" => " application/json" }.freeze # Iodine writes ':' without optional whitespace.
  SPACE = "\u0009-\u000d\u0020\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000\ufeff"
  TRIM = /\A[#{SPACE}]+|[#{SPACE}]+\z/
  POST_JSON = "json_object('id',p.id,'body',p.body,'created_at',p.created_at,
               'author',u.username,'like_count',(SELECT count(*) FROM likes WHERE post_id=p.id))"
  FROM_POSTS = "FROM posts p JOIN users u ON u.id=p.user_id"
  SQL = {
    health: "SELECT 1",
    feed: "SELECT '{\"posts\":[' || coalesce(group_concat(item ORDER BY created_at DESC,id DESC),'') || ']}'
           FROM (SELECT p.id,p.created_at,#{POST_JSON} AS item #{FROM_POSTS}
                 ORDER BY p.created_at DESC,p.id DESC LIMIT 20)",
    post: "SELECT json_object('post',#{POST_JSON}) #{FROM_POSTS} WHERE p.id=?",
    create: "INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING json_object('post',
             json_object('id',id,'body',body,'created_at',created_at,'author',?,'like_count',0))",
    like: "INSERT INTO likes(user_id,post_id) SELECT ?,id FROM posts WHERE id=?
           ON CONFLICT(user_id,post_id) DO NOTHING RETURNING post_id",
    exists: "SELECT 1 FROM posts WHERE id=?"
  }.freeze

  def initialize
    @started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    @secret = ENV.fetch("JWT_SECRET")
    @lock = Mutex.new
    @db = SQLite3::Database.new(ENV.fetch("SQLITE_PATH"))
    @db.execute_batch("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;
                       PRAGMA locking_mode=EXCLUSIVE; PRAGMA cache_size=500; PRAGMA mmap_size=536870912;")
    @queries = SQL.transform_values { |sql| @db.prepare(sql) }
  end

  def call(env)
    @lock.synchronize { catch(:response) { dispatch(env) } }
  rescue StandardError
    reply(JSON.generate(error: "internal server error"), 500)
  end

  def dispatch(env)
    method, path = env.values_at("REQUEST_METHOD", "PATH_INFO")
    if method == "GET"
      return health if path == "/health"
      return reply(query(:feed)) if path == "/feed"
    end
    if method == "POST" && path == "/posts"
      user, username = authenticate(env)
      raw = env.fetch("rack.input").read.force_encoding(Encoding::UTF_8)
      error(400, "malformed JSON body") unless raw.valid_encoding?
      payload = JSON.parse(raw)
      body = payload["body"] if payload.is_a?(Hash)
      error(400, "body is required") unless body.is_a?(String)
      body = body.gsub(TRIM, "")
      error(400, "body is required") if body.empty?
      error(400, "body must be at most 500 characters") if body.length > 500
      return reply(query(:create, user, body, username), 201)
    end
    route = /\A\/posts\/([^\/]+)(\/like)?\z/.match(path)
    if route && ((method == "GET" && !route[2]) || (method == "POST" && route[2]))
      user, = authenticate(env) if method == "POST"
      error(400, "invalid post id") unless positive_id?(route[1])
      id = route[1].to_i
      error(404, "post not found") if id > 9_223_372_036_854_775_807
      if method == "GET"
        document = query(:post, id)
        error(404, "post not found") unless document
        return reply(document)
      end
      added = query(:like, user, id)
      error(404, "post not found") unless added || query(:exists, id)
      return reply(JSON.generate(liked: true, already_liked: !added, post_id: id), added ? 201 : 200)
    end
    error(404, "not found")
  rescue JSON::ParserError
    error(400, "malformed JSON body")
  end

  def query(name, *arguments)
    statement = @queries.fetch(name)
    arguments.each_with_index { |value, index| statement.bind_param(index + 1, value) }
    row = statement.step
    statement.step if row && (name == :create || name == :like) # Commit RETURNING before responding.
    row&.first
  ensure
    statement&.reset!
  end

  def health
    query(:health)
    uptime = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - @started).to_i
    reply(JSON.generate(status: "ok", db: "ok", uptime_s: uptime))
  rescue SQLite3::Exception => exception
    reply(JSON.generate(status: "degraded", db: "unreachable", error: exception.message), 503)
  end

  def authenticate(env)
    bearer = env["HTTP_AUTHORIZATION"].to_s
    error(401, "missing bearer token") unless bearer.start_with?("Bearer ")
    parts = bearer.delete_prefix("Bearer ").split(".", -1)
    error(401, "invalid or expired token") unless parts.length == 3
    head, payload, signature = parts.map { |part| decode64(part) }
    expected = OpenSSL::HMAC.digest("SHA256", @secret, "#{parts[0]}.#{parts[1]}")
    valid = signature.bytesize == expected.bytesize && OpenSSL.fixed_length_secure_compare(signature, expected)
    error(401, "invalid or expired token") unless valid && head.valid_encoding? && payload.valid_encoding?
    head, payload = JSON.parse(head), JSON.parse(payload)
    valid = head.is_a?(Hash) && head["alg"] == "HS256" && payload.is_a?(Hash)
    now = Time.now.to_f
    valid &&= numeric_date?(payload["exp"]) && payload["exp"] > now
    valid &&= !payload.key?("nbf") || (numeric_date?(payload["nbf"]) && payload["nbf"] <= now)
    error(401, "invalid or expired token") unless valid
    error(401, "invalid token payload") unless positive_id?(payload["sub"]) &&
      payload["sub"].to_i <= 9_223_372_036_854_775_807 && payload["username"].is_a?(String)
    [payload["sub"].to_i, payload["username"]]
  rescue JSON::ParserError, ArgumentError
    error(401, "invalid or expired token")
  end

  def decode64(part)
    raise ArgumentError unless part.match?(/\A[A-Za-z0-9_-]+\z/) && part.length % 4 != 1
    (part.tr("-_", "+/") + "=" * ((-part.length) % 4)).unpack1("m0").force_encoding(Encoding::UTF_8)
  end

  def numeric_date?(value)
    value.is_a?(Numeric) && value.finite?
  end

  def positive_id?(value)
    value.is_a?(String) && value.match?(/\A[0-9]+\z/) && value.to_i.positive?
  end

  def reply(document, status = 200)
    [status, HEADERS, [document]]
  end

  def error(status, message)
    throw :response, reply(JSON.generate(error: message), status)
  end
end
