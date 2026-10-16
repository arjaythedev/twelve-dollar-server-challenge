# frozen_string_literal: true

require "async"
require_relative "app"
require_relative "http"

host = ENV.fetch("HOST", "127.0.0.1")
port = Integer(ENV.fetch("PORT", "3000"))
store = Store.new(ENV.fetch("SQLITE_PATH"))
app = App.new(store, ENV.fetch("JWT_SECRET"))
server = HTTPServer.new(app, host, port)

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
  $stderr.puts "listening on #{host}:#{port}"
end
