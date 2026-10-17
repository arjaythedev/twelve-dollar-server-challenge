# frozen_string_literal: true

require "async"
require_relative "app"
require_relative "http"

host = ENV.fetch("HOST", "127.0.0.1")
port = Integer(ENV.fetch("PORT", "3000"))
store = Store.new(ENV.fetch("SQLITE_PATH"))
app = App.new(store, ENV.fetch("JWT_SECRET"))
server = HTTPServer.new(app, host, port)

# Warm YJIT before listening: reads, a JWT verification and a body parse, 300 times each. Nothing is
# written: the post body fails validation after it is parsed, and the like has an invalid id.
require "openssl"
secret = ENV.fetch("JWT_SECRET")
claims = [{ sub: "1", username: "warmup", exp: Time.now.to_i + 3600 }.to_json].pack("m0").tr("+/", "-_").delete("=")
signing_input = "#{App::HS256_HEADER}.#{claims}"
signature = [OpenSSL::HMAC.digest("SHA256", secret, signing_input)].pack("m0").tr("+/", "-_").delete("=")
auth = "Authorization: Bearer #{signing_input}.#{signature}\r\n"
server.warm([
  "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n",
  "GET /posts/1 HTTP/1.1\r\nHost: x\r\n\r\n",
  "POST /posts HTTP/1.1\r\nHost: x\r\n#{auth}Content-Length: 11\r\n\r\n{\"body\":\"\"}",
  "POST /posts/0/like HTTP/1.1\r\nHost: x\r\n#{auth}\r\n"
], 300)

GC.compact
Process.warmup if Process.respond_to?(:warmup)

Async do |task|
  task.async do
    loop do
      sleep 1
      store.checkpoint
    end
  end

  server.run(task)
  compiled = RubyVM::YJIT.enabled? ? "#{RubyVM::YJIT.runtime_stats[:compiled_iseq_count]} iseqs compiled" : "YJIT off"
  $stderr.puts "listening on #{host}:#{port} (#{compiled})"
end
