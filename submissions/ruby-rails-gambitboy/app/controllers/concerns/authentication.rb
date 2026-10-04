module Authentication
  extend ActiveSupport::Concern

  BEARER_PREFIX = "Bearer "
  POSITIVE_INTEGER = /\A[1-9]\d*\z/

  included do
    before_action :require_authentication
  end

  class_methods do
    def allow_unauthenticated_access(**options)
      skip_before_action :require_authentication, **options
    end
  end

  private

  def require_authentication
    header = request.authorization.to_s
    return render_error("missing bearer token", :unauthorized) unless header.start_with?(BEARER_PREFIX)

    payload = decode_token(header.delete_prefix(BEARER_PREFIX))
    return render_error("invalid or expired token", :unauthorized) unless payload
    return render_error("invalid token payload", :unauthorized) unless valid_payload?(payload)

    Current.user_id = payload["sub"].to_i
    Current.username = payload["username"]
  end

  def decode_token(token)
    JWT.decode(token, Rails.configuration.x.jwt_secret, true, algorithm: "HS256").first
  rescue JWT::DecodeError
    nil
  end

  def valid_payload?(payload)
    payload.is_a?(Hash) &&
      payload["sub"].is_a?(String) &&
      payload["sub"].match?(POSITIVE_INTEGER) &&
      payload["username"].is_a?(String)
  end
end
