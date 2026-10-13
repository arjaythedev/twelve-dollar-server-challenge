import { Effect, Schema, SchemaTransformation } from "effect"
import { HttpRouter, HttpServerRespondable, HttpServerResponse } from "effect/http"
import { Auth } from "../Auth/Auth.ts"
import { errorResponse } from "../Http/errorResponse.ts"
import { Post, PostBody, PostId, Posts } from "./Posts.ts"

const invalidPostId = errorResponse(400, "invalid post id")

export class InvalidPostId extends Schema.TaggedError<InvalidPostId>()("InvalidPostId", {}) {
  [HttpServerRespondable.symbol]() {
    return invalidPostId
  }
}

const malformedJsonBody = errorResponse(400, "malformed JSON body")

export class MalformedJsonBody extends Schema.TaggedError<MalformedJsonBody>()("MalformedJsonBody", {}) {
  [HttpServerRespondable.symbol]() {
    return malformedJsonBody
  }
}

const bodyRequired = errorResponse(400, "body is required")

export class BodyRequired extends Schema.TaggedError<BodyRequired>()("BodyRequired", {}) {
  [HttpServerRespondable.symbol]() {
    return bodyRequired
  }
}

const bodyTooLong = errorResponse(400, "body must be at most 500 characters")

export class BodyTooLong extends Schema.TaggedError<BodyTooLong>()("BodyTooLong", {}) {
  [HttpServerRespondable.symbol]() {
    return bodyTooLong
  }
}

const PostIdParams = Schema.Struct({
  id: Schema.String.check(Schema.isPattern(/^[1-9][0-9]*$/)).pipe(
    Schema.decodeTo(PostId, SchemaTransformation.numberFromString)
  )
})

const CreatePostRequest = Schema.Struct({
  body: Schema.Trim.check(Schema.isNonEmpty())
})

const PostJson = Post.pipe(
  Schema.encodeKeys({ createdAt: "created_at", likeCount: "like_count" })
)

const FeedResponse = Schema.Struct({
  posts: Schema.Array(PostJson)
})

const PostResponse = Schema.Struct({
  post: PostJson
})

const LikeResponse = Schema.Struct({
  liked: Schema.Literal(true),
  alreadyLiked: Schema.Boolean,
  postId: PostId
}).pipe(
  Schema.encodeKeys({ alreadyLiked: "already_liked", postId: "post_id" })
)

const decodeJson = Schema.decodeUnknownEffect(Schema.fromJsonString(Schema.Unknown))
const decodeCreatePost = Schema.decodeUnknownEffect(CreatePostRequest)
const decodePostBody = Schema.decodeUnknownEffect(PostBody)

const encodeFeed = HttpServerResponse.schemaJson(FeedResponse)
const encodePost = HttpServerResponse.schemaJson(PostResponse)
const encodeLike = HttpServerResponse.schemaJson(LikeResponse)

const postIdParam = HttpRouter.schemaPathParams(PostIdParams).pipe(
  Effect.map(({ id }) => id),
  Effect.mapError(() => new InvalidPostId())
)

export const PostRoutes = HttpRouter.use(Effect.fn(function*(router) {
  const posts = yield* Posts
  const auth = yield* Auth

  yield* router.add("GET", "/feed", Effect.gen(function*() {
    const feed = yield* posts.feed

    return yield* encodeFeed({ posts: feed }).pipe(Effect.orDie)
  }))

  yield* router.add("GET", "/posts/:id", Effect.gen(function*() {
    const postId = yield* postIdParam
    const post = yield* posts.find(postId)

    return yield* encodePost({ post }).pipe(Effect.orDie)
  }))

  yield* router.add("POST", "/posts", (request) =>
    Effect.gen(function*() {
      const author = yield* auth.authenticate(request.headers.authorization)
      const text = yield* request.text

      const json = yield* decodeJson(text).pipe(Effect.mapError(() => new MalformedJsonBody()))
      const input = yield* decodeCreatePost(json).pipe(Effect.mapError(() => new BodyRequired()))
      const body = yield* decodePostBody(input.body).pipe(Effect.mapError(() => new BodyTooLong()))

      const post = yield* posts.create(author, body)

      return yield* encodePost({ post }, { status: 201 }).pipe(Effect.orDie)
    }))

  yield* router.add("POST", "/posts/:id/like", (request) =>
    Effect.gen(function*() {
      const user = yield* auth.authenticate(request.headers.authorization)
      const postId = yield* postIdParam

      const { alreadyLiked } = yield* posts.like(user.userId, postId)
      const status = alreadyLiked ? 200 : 201

      return yield* encodeLike({ liked: true, alreadyLiked, postId }, { status }).pipe(Effect.orDie)
    }))
}))
