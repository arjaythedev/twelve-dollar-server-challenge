threads_count = ENV.fetch("RAILS_MAX_THREADS", 3)
threads threads_count, threads_count

bind "tcp://#{ENV.fetch("HOST", "0.0.0.0")}:#{ENV.fetch("PORT", 3000)}"
persistent_timeout 75

plugin :tmp_restart

pidfile ENV["PIDFILE"] if ENV["PIDFILE"]
