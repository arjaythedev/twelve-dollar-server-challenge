require_relative "boot"

require "rails/all"

# Require the gems listed in Gemfile, including any gems
# you've limited to :test, :development, or :production.
Bundler.require(*Rails.groups)

module RubyRailsGambitboy
  class Application < Rails::Application
    # Initialize configuration defaults for originally generated Rails version.
    config.load_defaults 8.1

    # Please, add to the `ignore` list any other `lib` subdirectories that do
    # not contain `.rb` files, or that should not be reloaded or eager loaded.
    # Common ones are `templates`, `generators`, or `middleware`, for example.
    config.autoload_lib(ignore: %w[assets tasks])

    config.x.booted_at = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    config.x.jwt_secret = ENV["JWT_SECRET"]

    [
      Rack::Sendfile,
      ActionDispatch::Static,
      Rack::Runtime,
      Rack::MethodOverride,
      ActionDispatch::RequestId,
      ActionDispatch::RemoteIp,
      ActionDispatch::Cookies,
      ActionDispatch::Session::CookieStore,
      ActionDispatch::Flash,
      ActionDispatch::ContentSecurityPolicy::Middleware,
      Rack::ConditionalGet,
      Rack::ETag,
      Rack::TempfileReaper
    ].each { |middleware| config.middleware.delete(middleware) }

    # Configuration for the application, engines, and railties goes here.
    #
    # These settings can be overridden in specific environments using the files
    # in config/environments, which are processed later.
    #
    # config.time_zone = "Central Time (US & Canada)"
    # config.eager_load_paths << Rails.root.join("extras")
  end
end
