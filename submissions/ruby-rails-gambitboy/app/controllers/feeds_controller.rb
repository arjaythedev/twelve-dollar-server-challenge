class FeedsController < ApplicationController
  allow_unauthenticated_access

  def show
    posts = Post.with_details
                .newest_first
                .limit(20)
    render json: { posts: }
  end
end
