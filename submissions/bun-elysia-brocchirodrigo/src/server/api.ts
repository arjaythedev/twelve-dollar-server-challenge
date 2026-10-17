import { makeAuthenticate } from '@modules/auth/application/useCases/authenticate'
import { createHs256TokenVerifier } from '@modules/auth/infrastructure/crypto/hs256TokenVerifier'
import { healthRoutes } from '@modules/health/adapter/http/routes'
import { makeCheckHealth } from '@modules/health/application/useCases/checkHealth'
import { createSqliteHealthRepository } from '@modules/health/infrastructure/database/sqliteHealthRepository'
import { postsRoutes } from '@modules/posts/adapter/http/routes'
import { makeCreatePost } from '@modules/posts/application/useCases/createPost'
import { makeGetFeed } from '@modules/posts/application/useCases/getFeed'
import { makeGetPost } from '@modules/posts/application/useCases/getPost'
import { makeLikePost } from '@modules/posts/application/useCases/likePost'
import { createSqlitePostRepository } from '@modules/posts/infrastructure/database/sqlitePostRepository'
import { openDatabase } from '@server/config/database'
import { env } from '@server/config/env'
import { authMiddleware } from '@server/middlewares/auth'
import { errorHandler } from '@server/middlewares/errorHandler'
import { Elysia } from 'elysia'

const db = openDatabase(env.sqlitePath)
const posts = createSqlitePostRepository(db)
const auth = authMiddleware(makeAuthenticate(createHs256TokenVerifier(env.jwtSecret)))

export const api = new Elysia()
  .use(errorHandler)
  .use(healthRoutes(makeCheckHealth(createSqliteHealthRepository(db))))
  .use(
    postsRoutes(
      {
        getFeed: makeGetFeed(posts),
        getPost: makeGetPost(posts),
        createPost: makeCreatePost(posts),
        likePost: makeLikePost(posts),
      },
      auth,
    ),
  )
