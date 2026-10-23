threads 2, 2
workers 0
bind "tcp://#{ENV.fetch('HOST', '127.0.0.1')}:#{ENV.fetch('PORT', '3000')}"
persistent_timeout 75
queue_requests true # Keep idle connections and incomplete uploads in the reactor.
max_keep_alive 1_000_000_000 # Avoid the default 999-request cap during long pipelines.
