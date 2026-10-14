import { type PostNotFoundError, postNotFound } from '@modules/posts/domain/exceptions/postNotFoundError'
import type { PostRepository } from '@modules/posts/domain/repositories/postRepository'

type LikeResult = 'liked' | 'alreadyLiked'

export type LikePost = (userId: number, postId: number) => LikeResult | PostNotFoundError

export const makeLikePost =
  (posts: PostRepository): LikePost =>
  (userId, postId) => {
    if (posts.addLike(userId, postId)) return 'liked'
    return posts.exists(postId) ? 'alreadyLiked' : postNotFound
  }
