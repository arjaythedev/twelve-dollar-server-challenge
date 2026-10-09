# frozen_string_literal: true
# Rack does not expose every accepted HTTP socket; sample Linux process descriptors.
class ConnectionPressure
  def initialize(budget)
    @budget = [1, [budget, Process.getrlimit(:NOFILE).first - 128].min].max
    @next_sample, @retiring, @lock = 0.0, false, Mutex.new
  end

  def call
    @lock.synchronize do
      now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      if now >= @next_sample
        @next_sample = now + 0.1
        begin
          @retiring = Dir.each_child("/proc/self/fd").count >= @budget
        rescue Errno::EMFILE, Errno::ENFILE
          @retiring = true
        rescue SystemCallError
          # Preserve the last decision; /proc is absent on macOS.
        end
      end
      @retiring
    end
  end
end
