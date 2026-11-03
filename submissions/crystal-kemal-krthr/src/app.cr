# ----- constants ----- #

START      = Time.instant
JWT_SECRET = ENV["JWT_SECRET"]

# ----- database ----- #

db = DB.connect("sqlite3:#{URI.encode_www_form(ENV["SQLITE_PATH"])}")
db.exec("PRAGMA journal_mode = WAL")
db.exec("PRAGMA synchronous = NORMAL")
db.exec("PRAGMA foreign_keys = ON")
db.exec("PRAGMA busy_timeout = 5000")
db.exec("PRAGMA mmap_size = 1073741824")
db.exec("PRAGMA cache_size = -65536")
at_exit { db.close }

POST_COLUMNS = {id: Int64, body: String, created_at: String, author: String, like_count: Int64}
SELECT_POST  = <<-SQL
  SELECT p.id, p.body, p.created_at, u.username,
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
    FROM posts p JOIN users u ON u.id = p.user_id
  SQL
SELECT_FEED       = "#{SELECT_POST} ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
SELECT_POST_BY_ID = "#{SELECT_POST} WHERE p.id = ?"

def positive_id(value : String?) : Int64?
  return unless value && value.matches?(/\A[0-9]+\z/)
  id = value.to_i64?
  id if id && id > 0
end

# ----- auth ----- #

def authenticate(env : HTTP::Server::Context) : {Int64, String}?
  authorization = env.request.headers["Authorization"]?
  unless authorization && authorization.starts_with?("Bearer ")
    env.status(401).json({error: "missing bearer token"})
    return
  end

  begin
    token = authorization[7..]
    raise ArgumentError.new unless token.matches?(/\A[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\z/)
    header, payload, signature = token.split('.')
    algorithm = JSON.parse(Base64.decode_string(header)).as_h?.try(&.["alg"]?).try(&.as_s?)
    raise ArgumentError.new unless algorithm == "HS256"

    expected = OpenSSL::HMAC.digest(:sha256, JWT_SECRET, "#{header}.#{payload}")
    raise ArgumentError.new unless Crypto::Subtle.constant_time_compare(expected, Base64.decode(signature))

    claims = JSON.parse(Base64.decode_string(payload)).as_h?
    raise ArgumentError.new unless claims
    exp = claims["exp"]?
    expires_at = exp.try(&.as_i64?) || exp.try(&.as_f?)
    raise ArgumentError.new unless expires_at && expires_at > Time.utc.to_unix_f
  rescue ArgumentError | JSON::ParseException | Base64::Error
    env.status(401).json({error: "invalid or expired token"})
    return
  end

  user_id = positive_id(claims["sub"]?.try(&.as_s?))
  username = claims["username"]?.try(&.as_s?)
  unless user_id && username
    env.status(401).json({error: "invalid token payload"})
    return
  end

  {user_id, username}
end

# ----- routes ----- #

error 404 do |env|
  env.response.content_type = "application/json"
  {error: "not found"}.to_json
end

error 500 do |env|
  env.response.content_type = "application/json"
  {error: "internal server error"}.to_json
end

get "/health" do |env|
  begin
    db.query_one("SELECT 1", as: Int32)
    env.json({status: "ok", db: "ok", uptime_s: (Time.instant - START).total_seconds.to_i64})
  rescue ex
    env.status(503).json({status: "degraded", db: "unreachable", error: ex.message || "database error"})
  end
end

get "/feed" do |env|
  posts = db.query_all(SELECT_FEED, as: POST_COLUMNS)
  env.json({posts: posts})
end

get "/posts/:id" do |env|
  id = positive_id(env.params.url["id"]?)
  halt env.status(400).json({error: "invalid post id"}) unless id

  post = db.query_one?(SELECT_POST_BY_ID, id, as: POST_COLUMNS)
  halt env.status(404).json({error: "post not found"}) unless post

  env.json({post: post})
end

post "/posts" do |env|
  user = authenticate(env)
  next unless user

  begin
    body = JSON.parse(env.params.raw_body).as_h?.try(&.["body"]?).try(&.as_s?).try(&.strip)
  rescue JSON::ParseException
    halt env.status(400).json({error: "malformed JSON body"})
  end

  halt env.status(400).json({error: "body is required"}) if body.nil? || body.empty?
  halt env.status(400).json({error: "body must be at most 500 characters"}) if body.size > 500

  id, created_at = db.query_one("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
    user[0], body, as: {Int64, String})
  env.status(201).json({post: {id: id, body: body, created_at: created_at, author: user[1], like_count: 0}})
end

post "/posts/:id/like" do |env|
  user = authenticate(env)
  next unless user

  id = positive_id(env.params.url["id"]?)
  halt env.status(400).json({error: "invalid post id"}) unless id

  result = db.exec(<<-SQL, user[0], id)
    INSERT INTO likes (user_id, post_id)
      SELECT ?, id FROM posts WHERE id = ?
      ON CONFLICT (user_id, post_id) DO NOTHING
    SQL
  already_liked = result.rows_affected == 0
  if already_liked && !db.query_one?("SELECT 1 FROM posts WHERE id = ?", id, as: Int32)
    halt env.status(404).json({error: "post not found"})
  end

  env.status(already_liked ? 200 : 201).json({liked: true, already_liked: already_liked, post_id: id})
end
