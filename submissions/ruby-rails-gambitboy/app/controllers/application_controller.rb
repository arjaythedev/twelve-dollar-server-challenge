class ApplicationController < ActionController::API
  include Authentication

  wrap_parameters false

  rescue_from StandardError, with: :internal_server_error
  rescue_from ActiveRecord::RecordNotFound, with: :post_not_found
  rescue_from ActionDispatch::Http::Parameters::ParseError, with: :malformed_json_body

  private

  def render_error(message, status)
    render json: { error: message }, status:
  end

  def internal_server_error(exception)
    Rails.logger.error(exception.full_message)
    render_error("internal server error", :internal_server_error)
  end

  def post_not_found
    render_error("post not found", :not_found)
  end

  def malformed_json_body
    render_error("malformed JSON body", :bad_request)
  end
end
