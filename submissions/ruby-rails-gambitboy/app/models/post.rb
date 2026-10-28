class Post < ApplicationRecord
  include Likeable

  belongs_to :user

  attribute :author, :string
  attribute :like_count, :integer, default: 0

  normalizes :body, with: ->(body) { body.strip }

  validates :body, presence: { message: "body is required" },
                   length: { maximum: 500, message: "body must be at most 500 characters" }

  scope :with_details, -> {
    joins(:user)
      .select("posts.id, posts.body, posts.created_at, users.username AS author")
      .select("(SELECT count(*) FROM likes WHERE likes.post_id = posts.id) AS like_count")
  }
  scope :newest_first, -> { order(created_at: :desc, id: :desc) }

  def as_json(*)
    { id:, body:, created_at:, author:, like_count: }
  end
end
