# frozen_string_literal: true

require "async"
require "async/http/endpoint"
require "falcon/server"
require_relative "app"

# Last-resort guard for serving keep-alive clients directly: only when open descriptors get close to
# the process limit do responses carry `Connection: close`, so clients reconnect instead of the
# server failing to accept. Forced reconnects are expensive, so the budget sits just under the limit.
class Pressure
  attr_reader :strained

  def initialize(connections)
    soft = Process.getrlimit(:NOFILE).first
    @budget = [soft - 256, soft * 9 / 10].max
    @connections = connections # Falcon's live count of open client connections
    @strained = false
  end

  def sample
    @strained = @connections.value > @budget
  end
end

host = ENV.fetch("HOST", "127.0.0.1")
port = Integer(ENV.fetch("PORT", "3000"))
store = Store.new(ENV.fetch("SQLITE_PATH"))
utilization = Async::Utilization::Registry.new
pressure = Pressure.new(utilization.metric(:connections_active))
app = App.new(store, ENV.fetch("JWT_SECRET"), pressure)

# Idle keep-alive connections stay open 75 s, longer than Nginx's 60 s upstream keepalive_timeout.
endpoint = Async::HTTP::Endpoint.parse("http://#{host}:#{port}", reuse_address: true, timeout: 75)

GC.compact
Process.warmup if Process.respond_to?(:warmup)

Async do |task|
  task.async do
    loop do
      sleep 1
      store.checkpoint
      pressure.sample
    end
  end

  Falcon::Server.new(app, endpoint, utilization_registry: utilization).run
  $stderr.puts "listening on #{host}:#{port}"
end
