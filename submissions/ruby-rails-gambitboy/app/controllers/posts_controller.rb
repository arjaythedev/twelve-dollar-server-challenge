class PostsController < ApplicationController
  include PostIdentifiable

  allow_unauthenticated_access only: :show
  before_action -> { require_valid_post_id(params[:id]) }, only: :show

  def show
    post = Post.with_details.find(params[:id])
    render json: { post: }
  end

  def create
    return render_error("body is required", :bad_request) unless params[:body].is_a?(String)

    post = Post.new(user_id: Current.user_id, author: Current.username, body: params[:body])
    return render json: { post: }, status: :created if post.save
    raise ActiveRecord::RecordInvalid, post unless post.errors.include?(:body)

    render_error(post.errors[:body].first, :bad_request)
  end
end
