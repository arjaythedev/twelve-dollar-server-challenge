import type { Post } from '@modules/posts/domain/entities/post'
import type { PostRepository } from '@modules/posts/domain/repositories/postRepository'

const feedSize = 20

export type GetFeed = () => Post[]

export const makeGetFeed =
  (posts: PostRepository): GetFeed =>
  () =>
    posts.latest(feedSize)
