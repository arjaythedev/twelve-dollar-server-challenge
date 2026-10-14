import type { Post } from '@modules/posts/domain/entities/post'

export const presentPost = (post: Post) => ({
  id: post.id,
  body: post.body,
  created_at: post.createdAt,
  author: post.author,
  like_count: post.likeCount,
})

export const presentLike = (postId: number, alreadyLiked: boolean) => ({
  liked: true,
  already_liked: alreadyLiked,
  post_id: postId,
})
