import type { Post } from '@modules/posts/domain/entities/post'

export type NewPostRecord = { id: number; createdAt: string }

export type PostRepository = {
  latest: (limit: number) => Post[]
  findById: (id: number) => Post | null
  create: (authorId: number, body: string) => NewPostRecord
  addLike: (userId: number, postId: number) => boolean
  exists: (id: number) => boolean
}
