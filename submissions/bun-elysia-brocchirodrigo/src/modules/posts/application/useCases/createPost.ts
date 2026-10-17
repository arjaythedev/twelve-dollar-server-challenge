import type { Post } from '@modules/posts/domain/entities/post'
import { parsePostBody } from '@modules/posts/domain/entities/postBody'
import type { InvalidPostBodyError } from '@modules/posts/domain/exceptions/invalidPostBodyError'
import type { PostRepository } from '@modules/posts/domain/repositories/postRepository'

export type CreatePost = (authorId: number, authorName: string, body: string) => Post | InvalidPostBodyError

export const makeCreatePost =
  (posts: PostRepository): CreatePost =>
  (authorId, authorName, body) => {
    const text = parsePostBody(body)
    if (typeof text !== 'string') return text
    const { id, createdAt } = posts.create(authorId, text)
    return { id, body: text, createdAt, author: authorName, likeCount: 0 }
  }
