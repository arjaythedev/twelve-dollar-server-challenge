import { createPostBody, invalidPostId, isValidPostId, postIdParams } from '@modules/posts/adapter/http/dto'
import { presentLike, presentPost } from '@modules/posts/adapter/http/postPresenter'
import type { CreatePost } from '@modules/posts/application/useCases/createPost'
import type { GetFeed } from '@modules/posts/application/useCases/getFeed'
import type { GetPost } from '@modules/posts/application/useCases/getPost'
import type { LikePost } from '@modules/posts/application/useCases/likePost'
import { InvalidPostBodyError } from '@modules/posts/domain/exceptions/invalidPostBodyError'
import { PostNotFoundError } from '@modules/posts/domain/exceptions/postNotFoundError'
import type { AuthMiddleware } from '@server/middlewares/auth'
import { Elysia, status } from 'elysia'

export type PostsUseCases = {
  getFeed: GetFeed
  getPost: GetPost
  createPost: CreatePost
  likePost: LikePost
}

export const postsRoutes = ({ getFeed, getPost, createPost, likePost }: PostsUseCases, auth: AuthMiddleware) =>
  new Elysia({ name: 'posts' })
    .get('/feed', () => ({ posts: getFeed().map(presentPost) }))
    .get('/posts/:id', { params: postIdParams }, ({ params }) => {
      const post = getPost(Number(params.id))
      return post instanceof PostNotFoundError ? status(404, { error: post.message }) : { post: presentPost(post) }
    })
    .use(auth)
    .post('/posts', { body: createPostBody }, ({ body, user }) => {
      const post = createPost(user.id, user.username, body.body)
      if (post instanceof InvalidPostBodyError) return status(400, { error: post.message })
      return status(201, { post: presentPost(post) })
    })
    .post('/posts/:id/like', { parse: 'none' }, ({ params, user }) => {
      if (!isValidPostId(params.id)) return status(400, { error: invalidPostId })
      const postId = Number(params.id)
      const result = likePost(user.id, postId)
      if (result instanceof PostNotFoundError) return status(404, { error: result.message })
      return result === 'liked' ? status(201, presentLike(postId, false)) : presentLike(postId, true)
    })
