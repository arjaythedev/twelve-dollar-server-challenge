import type { Post } from '@modules/posts/domain/entities/post'
import { type PostNotFoundError, postNotFound } from '@modules/posts/domain/exceptions/postNotFoundError'
import type { PostRepository } from '@modules/posts/domain/repositories/postRepository'

export type GetPost = (id: number) => Post | PostNotFoundError

export const makeGetPost =
  (posts: PostRepository): GetPost =>
  (id) =>
    posts.findById(id) ?? postNotFound
