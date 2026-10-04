module Post::Likeable
  extend ActiveSupport::Concern

  included do
    has_many :likes
  end

  def like_by(user_id)
    Like.insert({ user_id:, post_id: id }, returning: %i[post_id])
        .rows
        .any?
  end
end
