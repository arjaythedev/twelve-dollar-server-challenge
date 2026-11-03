# frozen_string_literal: true

require "socket"
require_relative "epoll"

# A small HTTP/1.1 server on the Async reactor (io-event: io_uring or epoll), specialised for this API:
# it parses the request line and the four headers the API needs, and writes each response with one
# write. Requests are handled in order per connection (pipelining works); bodies use Content-Length.
#
# Idle keep-alive connections own no fiber: they sit in a kernel epoll set (edge-triggered, so a
# connection is registered once and never re-armed). One Async task waits for that set to become
# readable through the reactor, and a pool of worker fibers serves the connections that are ready.
# A fiber per connection would keep the stack pages each one touched while serving (deep SQLite
# calls), which costs ~70 KB per idle connection.
class HTTPServer
  READ_SIZE = 8192
  MAX_HEAD = 16 * 1024
  MAX_BODY = 1024 * 1024
  WORKERS = 256
  MAX_WORKERS = 4096 # the pool grows past WORKERS only if every worker is busy
  MAX_EVENTS = 1024

  REASONS = {
    200 => "OK", 201 => "Created", 400 => "Bad Request", 401 => "Unauthorized", 404 => "Not Found",
    411 => "Length Required", 413 => "Content Too Large", 431 => "Request Header Fields Too Large",
    500 => "Internal Server Error", 503 => "Service Unavailable"
  }.freeze
  # Response heads up to "content-length: "; the length, a blank line and the body follow.
  def self.heads(extra)
    REASONS.to_h { |code, reason| [code, "HTTP/1.1 #{code} #{reason}\r\n#{extra}content-type: application/json\r\ncontent-length: ".freeze] }.freeze
  end
  KEEP = heads("")
  KEEP_10 = heads("connection: keep-alive\r\n") # HTTP/1.0 clients that asked for keep-alive
  CLOSE = heads("connection: close\r\n")
  END_HEAD = "\r\n\r\n"
  CRLF = "\r\n"
  # The only request headers the API needs. Names are told apart by length. The value is taken
  # greedily and trailing blanks are trimmed afterwards: a lazy `[^\r\n]*?` with a lookahead
  # backtracks along a 200-byte bearer token and took 5.7 µs on a POST head, against 2.9 µs now.
  HEADER = /\r\n(content-length|transfer-encoding|connection|authorization)[ \t]*:[ \t]*([^\r\n]*)/i
  TRAILING_BLANKS = /[ \t]+\z/
  DIGITS = /\A[0-9]+\z/
  CLOSE_TOKEN = /\bclose\b/i
  KEEP_ALIVE_TOKEN = /\bkeep-alive\b/i
  HTTP_11 = "HTTP/1.1"
  HTTP_10 = "HTTP/1.0"
  EMPTY = ""

  def initialize(app, host, port, connection_budget: nil)
    @app = app
    @server = TCPServer.new(host, port)
    @server.listen(65_535) # the kernel caps this at net.core.somaxconn
    # Accepted sockets inherit TCP_NODELAY from the listener on Linux: no setsockopt per connection.
    @server.setsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY, 1)
    soft = Process.getrlimit(:NOFILE).first
    # Last-resort guard: only close to the descriptor limit do responses carry `Connection: close`.
    @budget = connection_budget || [soft - 256, soft * 9 / 10].max
    @open = 0
  end

  attr_reader :open

  def run(task)
    @task = task
    @epfd = Epoll.create
    @event = Epoll.event(Epoll::IN | Epoll::RDHUP | Epoll::ET)
    @sockets = [] # by descriptor
    @state = [] # by descriptor: IDLE, BUSY (queued or being served) or AGAIN (busy, and new data came)
    @partial = {} # descriptor => bytes of a request that hasn't fully arrived
    @ready = Thread::Queue.new
    @workers = @waiting = 0
    WORKERS.times { add_worker }
    task.async { dispatch }
    task.async { accept_loop }
  end

  # Runs each request through the parser and the app `times` times over a socket pair, before the
  # reactor starts, so YJIT has compiled the whole request path when the first client arrives.
  def warm(requests, times)
    near, far = UNIXSocket.pair
    times.times do
      requests.each do |request|
        drain(near, request.b)
        far.read_nonblock(1 << 20, exception: false)
      end
    end
    near.close
    far.close
  end

  private

  def accept_loop
    loop do
      socket = @server.accept_nonblock(exception: false)
      if socket == :wait_readable
        @server.wait_readable
        next
      end
      @open += 1
      fd = socket.fileno
      @sockets[fd] = socket
      @state[fd] = IDLE
      Epoll.control(@epfd, Epoll::CTL_ADD, fd, @event)
    rescue Errno::EMFILE, Errno::ENFILE, Errno::ENOBUFS, Errno::ENOMEM
      sleep 0.01 # out of descriptors: let responses retire connections, then accept again
    rescue Errno::ECONNABORTED, Errno::EPROTO
      next
    end
  end

  # --- connections ----------------------------------------------------------------------

  IDLE = 0
  BUSY = 1
  AGAIN = 2

  # Waits (through the reactor) for the epoll set to have ready connections, then queues them.
  # A connection that is already queued or being served is only flagged, so one worker serves it
  # at a time (requests stay in order) and the worker reads it again before letting it go idle.
  def dispatch
    poller = IO.for_fd(@epfd, autoclose: false)
    events = Epoll.events(MAX_EVENTS)
    loop do
      poller.wait_readable
      Epoll.ready(@epfd, events, MAX_EVENTS) do |fd|
        case @state[fd]
        when IDLE
          @state[fd] = BUSY
          @ready.push(fd)
          add_worker if @waiting < @ready.size && @workers < MAX_WORKERS
        when BUSY
          @state[fd] = AGAIN
        end
      end
    end
  end

  def add_worker
    @workers += 1
    @task.async { work }
  end

  def work
    io_buffer = IO::Buffer.new(READ_SIZE)
    loop do
      @waiting += 1
      fd = @ready.pop
      @waiting -= 1
      service(fd, io_buffer)
    rescue StandardError => error
      Console.error(self, error)
    end
  end

  # Edge-triggered: a read shorter than READ_SIZE emptied the socket, and anything that arrives
  # after it raises a new edge, which `dispatch` turns into AGAIN while this worker still holds fd.
  #
  # The first read goes through the scheduler, so with io_uring the reads of every connection that
  # became ready in a reactor pass are submitted together. That read waits for data, so it is only
  # used where data is expected: a connection whose new data was already read in an earlier pass
  # (a stale edge) would hold this worker until its client sends again, which is why the pool can
  # grow. Re-reads after AGAIN, and reads past a full buffer, don't wait.
  def service(fd, io_buffer)
    socket = @sockets[fd] or return
    wait = true
    loop do
      @state[fd] = BUSY
      buffer = @partial.delete(fd)
      loop do
        data = if wait
                 wait = false
                 length = Fiber.scheduler.io_read(socket, io_buffer, 1, 0)
                 io_buffer.get_string(0, length) if length.positive?
               else
                 socket.read_nonblock(READ_SIZE, exception: false)
               end
        break if data == :wait_readable
        return disconnect(fd, socket) unless data

        buffer = buffer ? buffer << data : data
        break if data.bytesize < READ_SIZE
      end

      if buffer
        rest = drain(socket, buffer) or return disconnect(fd, socket)
        @partial[fd] = rest unless rest.empty?
      end
      next if @state[fd] == AGAIN

      @state[fd] = IDLE
      return
    end
  rescue IOError, SystemCallError
    disconnect(fd, socket)
  end

  def disconnect(fd, socket)
    @sockets[fd] = nil
    @state[fd] = nil
    @partial.delete(fd)
    @open -= 1
    socket.close unless socket.closed?
  end

  # --- HTTP/1.1 ---------------------------------------------------------------------------

  # Answers every complete request in `buffer`, in order. Returns the unconsumed tail (possibly
  # empty), or nil when the connection must be closed.
  def drain(socket, buffer)
    position = 0
    size = buffer.bytesize
    while position < size
      head_end = buffer.byteindex(END_HEAD, position)
      unless head_end
        return reject(socket, 431) if size - position > MAX_HEAD

        break
      end

      line_end = buffer.byteindex(CRLF, position)
      space = buffer.byteindex(" ", position)
      space2 = space && buffer.byteindex(" ", space + 1)
      return reject(socket, 400) unless space2 && space > position && space2 < line_end

      version = buffer.byteslice(space2 + 1, line_end - space2 - 1)
      http10 = version == HTTP_10
      return reject(socket, 400) unless http10 || version == HTTP_11

      method = buffer.byteslice(position, space - position)
      query = buffer.byteindex("?", space + 1)
      path_end = query && query < space2 ? query : space2
      path = buffer.byteslice(space + 1, path_end - space - 1)

      length = 0
      authorization = nil
      close = http10
      if line_end < head_end
        buffer.byteslice(line_end, head_end - line_end).scan(HEADER) do
          name = Regexp.last_match(1)
          value = Regexp.last_match(2)
          value = value.sub(TRAILING_BLANKS, "") if value.end_with?(" ", "\t")
          case name.bytesize
          when 14 # content-length
            return reject(socket, 400) unless DIGITS.match?(value)

            length = value.to_i
          when 17 # transfer-encoding: request bodies must use Content-Length
            return reject(socket, 411)
          when 10 # connection
            if CLOSE_TOKEN.match?(value) then close = true
            elsif http10 && KEEP_ALIVE_TOKEN.match?(value) then close = false
            end
          else # authorization
            authorization = value
          end
        end
      end
      return reject(socket, 413) if length > MAX_BODY

      body_start = head_end + 4
      break if size < body_start + length # the body hasn't fully arrived yet

      body = buffer.byteslice(body_start, length) if length.positive?
      status, document = @app.call(method, path, authorization, body)
      close ||= @open > @budget
      respond(socket, close ? CLOSE : (http10 ? KEEP_10 : KEEP), status, document)
      return nil if close

      position = body_start + length
    end

    if position.zero? then buffer
    elsif position >= size then EMPTY
    else buffer.byteslice(position, size - position)
    end
  end

  def reject(socket, status)
    respond(socket, CLOSE, status, %({"error":"#{REASONS[status].downcase}"}))
    nil
  end

  def respond(socket, heads, status, document)
    head = heads[status] || heads[500]
    out = String.new(head, capacity: head.bytesize + 8 + document.bytesize)
    out << document.bytesize.to_s << END_HEAD << document
    write(socket, out)
  end

  # Through the fiber scheduler: with io_uring, every worker's write is queued and submitted together.
  def write(socket, data)
    socket.write(data)
  end
end
