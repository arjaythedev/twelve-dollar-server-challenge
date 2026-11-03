module PostIdentifiable
  extend ActiveSupport::Concern

  POST_ID = /\A[1-9]\d*\z/

  private

  def require_valid_post_id(id)
    render_error("invalid post id", :bad_request) unless id.match?(POST_ID)
  end
end
