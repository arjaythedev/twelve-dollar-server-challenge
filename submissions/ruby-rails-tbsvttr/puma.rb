threads 2, 2
workers 0
bind "tcp://#{ENV.fetch('HOST', '127.0.0.1')}:#{ENV.fetch('PORT', '3000')}"
persistent_timeout 75
