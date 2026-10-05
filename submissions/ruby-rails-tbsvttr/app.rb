# frozen_string_literal: true
require "rails"
require "action_controller/railtie"
require "sqlite3"
require "jwt"
ActionDispatch::Request.parameter_parsers = {} # Parse JSON only after authentication.

class FeedApp < Rails::Application
  config.api_only = true
  config.eager_load = true
  config.enable_reloading = false
  config.public_file_server.enabled = false
  config.hosts.clear
  config.logger = Logger.new(File::NULL)
  config.log_level = :warn
  config.secret_key_base = ENV.fetch("JWT_SECRET")
  [Rack::Sendfile, Rack::Runtime, Rack::ETag, Rack::ConditionalGet, Rails::Rack::Logger,
   ActionDispatch::RequestId, ActionDispatch::RemoteIp, ActionDispatch::Callbacks].each do |middleware|
    config.middleware.delete middleware
  end
end

class FeedController < ActionController::Metal
  include AbstractController::Callbacks
  include ActionController::Rescue
  STARTED = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  SECRET = ENV.fetch("JWT_SECRET")
  QUERY_LOCK = Mutex.new
  DB = SQLite3::Database.new(ENV.fetch("SQLITE_PATH"))
  DB.execute_batch("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;
                    PRAGMA locking_mode=EXCLUSIVE; PRAGMA cache_size=500; PRAGMA mmap_size=536870912;")
  POST_JSON = "json_object('id',p.id,'body',p.body,'created_at',p.created_at,
                'author',u.username,'like_count',(SELECT count(*) FROM likes WHERE post_id=p.id))"
  FROM_POSTS = "FROM posts p JOIN users u ON u.id=p.user_id"
  QUERIES = {
    health: "SELECT 1",
    feed: "SELECT json_object('posts',json_group_array(#{POST_JSON} ORDER BY p.created_at DESC,p.id DESC))
           FROM (SELECT * FROM posts ORDER BY created_at DESC,id DESC LIMIT 20) p
           JOIN users u ON u.id=p.user_id",
    post: "SELECT json_object('post',#{POST_JSON}) #{FROM_POSTS} WHERE p.id=?",
    create: "INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING json_object('post',
             json_object('id',id,'body',body,'created_at',created_at,'author',?,'like_count',0))",
    like: "INSERT INTO likes(user_id,post_id) SELECT ?,id FROM posts WHERE id=?
           ON CONFLICT(user_id,post_id) DO NOTHING RETURNING post_id",
    exists: "SELECT 1 FROM posts WHERE id=?"
  }.transform_values { |sql| DB.prepare(sql) }
  SPACE = "\u0009-\u000d\u0020\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000\ufeff"
  TRIM = /\A[#{SPACE}]+|[#{SPACE}]+\z/
  before_action :authenticate, only: [:create, :like]
  before_action :identify, only: [:post, :like]
  around_action ->(_controller, action) { QUERY_LOCK.synchronize(&action) }
  rescue_from StandardError, with: -> { error(500, "internal server error") }

  def health
    query(:health)
    uptime = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - STARTED).to_i
    reply(JSON.generate(status: "ok", db: "ok", uptime_s: uptime))
  rescue SQLite3::Exception => exception
    reply(JSON.generate(status: "degraded", db: "unreachable", error: exception.message), 503)
  end

  def feed
    reply(query(:feed))
  end

  def post
    document = query(:post, @id)
    document ? reply(document) : error(404, "post not found")
  end

  def create
    document = request.raw_post.dup.force_encoding(Encoding::UTF_8)
    return error(400, "malformed JSON body") unless document.valid_encoding?
    payload = JSON.parse(document)
    body = payload["body"] if payload.is_a?(Hash)
    return error(400, "body is required") unless body.is_a?(String)
    body = body.gsub(TRIM, "")
    return error(400, "body is required") if body.empty?
    return error(400, "body must be at most 500 characters") if body.length > 500
    reply(query(:create, @user_id, body, @username), 201)
  rescue JSON::ParserError
    error(400, "malformed JSON body")
  end

  def like
    added = query(:like, @user_id, @id)
    return error(404, "post not found") unless added || query(:exists, @id)
    reply(JSON.generate(liked: true, already_liked: !added, post_id: @id), added ? 201 : 200)
  end

  def missing
    error(404, "not found")
  end

  private

  def query(name, *arguments)
    statement = QUERIES.fetch(name)
    arguments.each_with_index { |value, index| statement.bind_param(index + 1, value) }
    row = statement.step
    statement.step if row # Each query returns at most one row; reach SQLITE_DONE before acknowledging writes.
    row&.first
  ensure
    statement&.reset!
  end

  def reply(document, status = 200)
    self.status = status
    self.content_type = "application/json"
    self.response_body = document
  end

  def error(status, message)
    reply(JSON.generate(error: message), status)
  end

  def authenticate
    header = request.authorization.to_s
    return error(401, "missing bearer token") unless header.start_with?("Bearer ")
    token = header.delete_prefix("Bearer ")
    payload, decoded_header = JWT.decode(token, SECRET, true, algorithm: "HS256", required_claims: ["exp"])
    valid_utf8 = token.split(".", 3).first(2).all? do |segment|
      Base64.urlsafe_decode64(segment).force_encoding(Encoding::UTF_8).valid_encoding?
    end
    return error(401, "invalid or expired token") unless valid_utf8 && decoded_header["alg"] == "HS256"
    valid_dates = payload.is_a?(Hash) && payload["exp"].is_a?(Numeric)
    valid_dates &&= !payload.key?("nbf") || payload["nbf"].is_a?(Numeric)
    unless valid_dates
      return error(401, "invalid or expired token")
    end
    unless positive_id?(payload["sub"]) && payload["username"].is_a?(String)
      return error(401, "invalid token payload")
    end
    @user_id, @username = payload["sub"].to_i, payload["username"]
  rescue JWT::DecodeError, ArgumentError, TypeError, NoMethodError, RangeError
    error(401, "invalid or expired token")
  end

  def identify
    id = request.path_parameters[:id]
    return error(400, "invalid post id") unless positive_id?(id)
    @id = id.to_i
  end

  def positive_id?(value)
    value.is_a?(String) && value.match?(/\A[0-9]+\z/) && value.to_i.positive?
  end
end

FeedApp.initialize!
FeedApp.routes.draw do
  get "/health", to: "feed#health", format: false
  get "/feed", to: "feed#feed", format: false
  get "/posts/:id", to: "feed#post", format: false, constraints: { id: /[^\/]+/ }
  post "/posts", to: "feed#create", format: false
  post "/posts/:id/like", to: "feed#like", format: false, constraints: { id: /[^\/]+/ }
  match "(*path)", to: "feed#missing", via: :all, format: false
end
