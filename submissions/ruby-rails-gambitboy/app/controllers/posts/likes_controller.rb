class Posts::LikesController < ApplicationController
  include PostIdentifiable

  before_action -> { require_valid_post_id(params[:post_id]) }

  def create
    post = Post.select(:id).find(params[:post_id])
    liked = post.like_by(Current.user_id)
    render json: { liked: true, already_liked: !liked, post_id: post.id }, status: liked ? :created : :ok
  end
end
