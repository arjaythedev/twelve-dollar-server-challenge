# frozen_string_literal: true

require_relative "http"
require_relative "uring"

# Client connections are io_uring direct descriptors (see uring.rb), so they don't count against the
# process descriptor limit: each ring holds up to RLIMIT_NOFILE of them, and there are RINGS rings.
# Every connection has one multishot receive armed for its lifetime, filled from the ring's provided
# buffers; one Async task per ring waits for the ring's eventfd through the reactor and turns
# completions into connection state. Worker fibers serve the connections that have new bytes, and
# their responses are queued as sends that the next submit hands to the kernel together. No
# connection waits in `accept` or reconnects for want of a descriptor: the process itself keeps
# about 15 descriptors open.
class UringServer < HTTPServer
  RINGS = Integer(ENV.fetch("URING_RINGS", "2"))
  SLOT_BITS = 16 # connection id = ring << SLOT_BITS | slot
  SLOT_MASK = (1 << SLOT_BITS) - 1
  BUFFERS = 4096 # provided receive buffers per ring
  BUFFER_SIZE = 2048
  # user_data = connection id << 3 | operation
  ACCEPT = 0
  RECV = 1
  SEND = 2
  CANCEL = 3
  CLOSE_SLOT = 4
  ENOBUFS = 105
  ENFILE = 23


  # Raises LoadError (no liburing-ffi) or SystemCallError (io_uring unavailable or restricted)
  # before the listener is bound, so the caller can fall back to EpollServer.
  def initialize(app, host, port)
    slots = [Process.getrlimit(:NOFILE).first, SLOT_MASK].min
    @rings = Array.new(RINGS) do
      Uring::Ring.new(entries: 4096, completions: 32_768, slots: slots, buffers: BUFFERS, buffer_size: BUFFER_SIZE)
    end
    super(app, host, port, slots * RINGS)
  end

  def run(task)
    @task = task
    # By connection id:
    @input = [] # bytes received and not yet served
    @busy = [] # queued for or held by a worker
    @sending = [] # the send in flight (its memory), and its length and offset
    @send_length = []
    @send_offset = []
    @out = [] # responses waiting for the send in flight
    @closing = [] # close once the sends are done
    @cancelled = [] # the receive was asked to end
    @dead = [] # the receive has ended
    @accepting = Array.new(RINGS, false)
    @ready = Thread::Queue.new
    WORKERS.times { add_worker }
    @rings.each_index { |index| task.async { pump(index) } }
  end

  private

  def accept(index)
    @accepting[index] = true
    @rings[index].accept(@server.fileno, (index << SLOT_BITS) << 3 | ACCEPT)
  end

  # Waits (through the reactor) for the ring's eventfd, then handles every completion and hands the
  # resulting submissions to the kernel in one call.
  def pump(index)
    ring = @rings[index]
    eventfd = IO.for_fd(ring.eventfd, autoclose: false)
    accept(index)
    ring.submit
    loop do
      eventfd.wait_readable
      eventfd.read_nonblock(8, exception: false)
      ring.each_completion { |data, result, flags| complete(ring, index, data >> 3, data & 7, result, flags) }
      ring.submit
    rescue StandardError => error
      Console.error(self, error)
    end
  end

  def complete(ring, index, id, operation, result, flags)
    case operation
    when RECV
      if result.positive?
        data = ring.take_buffer(flags >> Uring::CQE_BUFFER_SHIFT, result)
        if (input = @input[id]) then input << data else @input[id] = data end
        unless @busy[id]
          @busy[id] = true
          @ready.push(id)
          add_worker if @waiting < @ready.size && @workers < MAX_WORKERS
        end
        ring.receive(id & SLOT_MASK, id << 3 | RECV) if flags.nobits?(Uring::CQE_F_MORE)
      elsif result == -ENOBUFS # out of provided buffers: they come back with this pass's submit
        ring.receive(id & SLOT_MASK, id << 3 | RECV)
      else # end of stream, an error, or cancelled
        @dead[id] = true
        finish(id)
      end
    when SEND
      sent(id, result)
    when ACCEPT
      if result >= 0
        id = index << SLOT_BITS | result
        @open += 1
        ring.receive(result, id << 3 | RECV)
      end
      accept(index) if flags.nobits?(Uring::CQE_F_MORE) && result != -ENFILE
      @accepting[index] = false if result == -ENFILE
    end
    # CANCEL and CLOSE_SLOT complete only when they fail: the receive ends or the slot is freed anyway.
  end

  def work
    loop do
      @waiting += 1
      id = @ready.pop
      @waiting -= 1
      service(id)
      flush if @ready.empty?
    rescue StandardError => error
      Console.error(self, error)
    end
  end

  # Serves every complete request received on the connection, including bytes that arrive while it
  # is being served; a partial request stays in @input until the rest arrives.
  def service(id)
    while (buffer = @input[id])
      @input[id] = nil
      rest = drain(id, buffer) unless @closing[id]
      unless rest
        @closing[id] = true
        break
      end
      next if rest.empty?

      more = @input[id]
      @input[id] = more ? rest << more : rest
      break unless more
    end
    @busy[id] = false
    finish(id)
  rescue StandardError
    @busy[id] = false
    @closing[id] = true
    finish(id)
    raise
  end

  def flush
    @rings.each { |ring| ring.submit if ring.pending? }
  end

  # Moves the connection toward closing once nothing holds it: a closing connection's receive is
  # cancelled, and the slot of one whose receive has ended is freed.
  def finish(id)
    return if @busy[id] || @sending[id]

    if @dead[id]
      close_slot(id)
    elsif @closing[id] && !@cancelled[id]
      @cancelled[id] = true
      @rings[id >> SLOT_BITS].cancel(id << 3 | RECV, id << 3 | CANCEL)
    end
  end

  def close_slot(id)
    index = id >> SLOT_BITS
    @rings[index].close(id & SLOT_MASK, id << 3 | CLOSE_SLOT)
    @input[id] = @out[id] = nil
    @closing[id] = @cancelled[id] = @dead[id] = false
    @open -= 1
    accept(index) unless @accepting[index]
  end

  def send_now(id, data)
    length = data.bytesize
    memory = FFI::MemoryPointer.new(:char, length, false)
    memory.put_bytes(0, data)
    @sending[id] = memory
    @send_length[id] = length
    @send_offset[id] = 0
    @rings[id >> SLOT_BITS].send_data(id & SLOT_MASK, memory, length, id << 3 | SEND)
  end

  def sent(id, result)
    memory = @sending[id]
    if result.negative?
      @sending[id] = @out[id] = nil
      @closing[id] = true
      return finish(id)
    end

    offset = @send_offset[id] + result
    if offset < @send_length[id] # a short send: the rest goes next
      @send_offset[id] = offset
      @rings[id >> SLOT_BITS].send_data(id & SLOT_MASK, memory + offset, @send_length[id] - offset, id << 3 | SEND)
    elsif (pending = @out[id])
      @out[id] = nil
      send_now(id, pending)
    else
      @sending[id] = nil
      finish(id)
    end
  end

  # Responses on one connection go out in order: while a send is in flight, later ones wait in @out.
  # The warm-up writes to a socket pair instead.
  def write(id, data)
    return id.write(data) unless id.is_a?(Integer)

    if @sending[id]
      if (pending = @out[id]) then pending << data else @out[id] = data end
    else
      send_now(id, data)
    end
  end
end
