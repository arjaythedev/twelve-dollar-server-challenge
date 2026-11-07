import { Context, Effect, Layer, Match, Schema } from "effect"
import { HttpServerRespondable } from "effect/http"
import { errorResponse } from "../Http/errorResponse.ts"
import { PostRepo, type PostRepoError } from "./PostRepo.ts"

export const PostId = Schema.Int.check(Schema.isGreaterThan(0)).pipe(Schema.brand("PostId"))
export type PostId = typeof PostId.Type

const maxBodyLength = 500

const codePointLength = (text: string) => Array.from(text).length

export const PostBody = Schema.String.check(
  Schema.isTrimmed(),
  Schema.isNonEmpty(),
  Schema.makeFilter((body: string) => codePointLength(body) <= maxBodyLength)
).pipe(
  Schema.brand("PostBody")
)
export type PostBody = typeof PostBody.Type

export const Post = Schema.Struct({
  id: Schema.Int,
  body: Schema.String,
  createdAt: Schema.String,
  author: Schema.String,
  likeCount: Schema.Int
})
export type Post = typeof Post.Type

const postNotFound = errorResponse(404, "post not found")

export class PostNotFound extends Schema.TaggedError<PostNotFound>()("PostNotFound", {
  postId: PostId
}) {
  [HttpServerRespondable.symbol]() {
    return postNotFound
  }
}

const postsUnavailable = errorResponse(500, "internal server error")

export class PostsUnavailable extends Schema.TaggedError<PostsUnavailable>()("PostsUnavailable", {
  cause: Schema.Defect()
}) {
  [HttpServerRespondable.symbol]() {
    return postsUnavailable
  }
}

const unavailable = (error: PostRepoError) => new PostsUnavailable({ cause: error })

export class Posts extends Context.Service<Posts, {
  readonly feed: Effect.Effect<ReadonlyArray<Post>, PostsUnavailable>
  find(postId: PostId): Effect.Effect<Post, PostNotFound | PostsUnavailable>
  create(author: { readonly userId: number; readonly username: string }, body: PostBody): Effect.Effect<Post, PostsUnavailable>
  like(userId: number, postId: PostId): Effect.Effect<{ readonly alreadyLiked: boolean }, PostNotFound | PostsUnavailable>
}>()("tdsc/Posts/Posts") {
  static readonly layer = Layer.effect(
    Posts,
    Effect.gen(function*() {
      const repo = yield* PostRepo

      const feed = repo.feed.pipe(
        Effect.mapError(unavailable),
        Effect.withSpan("Posts.feed")
      )

      const find = Effect.fn("Posts.find")((postId: PostId) =>
        repo.findById(postId).pipe(
          Effect.mapError(unavailable),
          Effect.flatMap(Effect.fromOption(() => new PostNotFound({ postId })))
        )
      )

      const create = Effect.fn("Posts.create")(function*(
        author: { readonly userId: number; readonly username: string },
        body: PostBody
      ) {
        const created = yield* repo.insert({ userId: author.userId, body }).pipe(
          Effect.mapError(unavailable)
        )

        return { id: created.id, body, createdAt: created.createdAt, author: author.username, likeCount: 0 }
      })

      const like = Effect.fn("Posts.like")((userId: number, postId: PostId) =>
        repo.like({ userId, postId }).pipe(
          Effect.mapError(unavailable),
          Effect.flatMap((outcome) =>
            Match.value(outcome).pipe(
              Match.when("Liked", () => Effect.succeed({ alreadyLiked: false })),
              Match.when("AlreadyLiked", () => Effect.succeed({ alreadyLiked: true })),
              Match.when("PostMissing", () => Effect.fail(new PostNotFound({ postId }))),
              Match.exhaustive
            )
          )
        )
      )

      return Posts.of({ feed, find, create, like })
    })
  ).pipe(
    Layer.provide(PostRepo.layer)
  )
}
