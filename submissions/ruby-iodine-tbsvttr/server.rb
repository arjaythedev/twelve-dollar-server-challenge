# frozen_string_literal: true
require "iodine"
require_relative "app"

Iodine.threads = 1
Iodine.workers = 1
Iodine.listen(service: :http, address: ENV.fetch("HOST", "127.0.0.1"),
              port: ENV.fetch("PORT", "3000"), handler: FeedApp.new,
              timeout: 75, ping: 75, log: false)
Iodine.start
