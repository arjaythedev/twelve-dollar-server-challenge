class Like < ApplicationRecord
  self.primary_key = %i[user_id post_id]

  belongs_to :user
  belongs_to :post
end
