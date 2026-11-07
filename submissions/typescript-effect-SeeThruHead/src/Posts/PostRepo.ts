import { desc, eq, sql } from "drizzle-orm"
import { Array as Arr, Context, Effect, Layer, Option, Request, RequestResolver, Schema } from "effect"
import { SqlSchema } from "effect/sql"
import { Database } from "../Database/Database.ts"
import { currentTimestamp, likes, posts, users } from "../Database/schema.ts"

export class PostRepoError extends Schema.TaggedError<PostRepoError>()("PostRepoError", {
  cause: Schema.Defect()
}) {}

const repoError = (cause: unknown) => new PostRepoError({ cause })

const Timestamp = Schema.String.check(Schema.isPattern(/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/))

const PostRow = Schema.Struct({
  id: Schema.Int,
  body: Schema.String,
  createdAt: Timestamp,
  author: Schema.String,
  likeCount: Schema.Int
})
type PostRow = typeof PostRow.Type

const CreatedPost = Schema.Struct({
  id: Schema.Int,
  createdAt: Timestamp
})
type CreatedPost = typeof CreatedPost.Type

const NewPost = Schema.Struct({
  userId: Schema.Int,
  body: Schema.String
})

const NewLike = Schema.Struct({
  userId: Schema.Int,
  postId: Schema.Int
})

const LikeOutcome = Schema.Literals(["Liked", "AlreadyLiked", "PostMissing"])
type LikeOutcome = typeof LikeOutcome.Type

class InsertPost extends Request.TaggedClass("InsertPost")<
  { readonly userId: number; readonly body: string },
  CreatedPost,
  PostRepoError
> {}

class InsertLike extends Request.TaggedClass("InsertLike")<
  { readonly userId: number; readonly postId: number },
  LikeOutcome,
  PostRepoError
> {}

const maxWritesPerCommit = 512

export class PostRepo extends Context.Service<PostRepo, {
  readonly feed: Effect.Effect<ReadonlyArray<PostRow>, PostRepoError>
  findById(id: number): Effect.Effect<Option.Option<PostRow>, PostRepoError>
  insert(input: { readonly userId: number; readonly body: string }): Effect.Effect<CreatedPost, PostRepoError>
  like(input: { readonly userId: number; readonly postId: number }): Effect.Effect<LikeOutcome, PostRepoError>
}>()("tdsc/Posts/PostRepo") {
  static readonly layer = Layer.effect(
    PostRepo,
    Effect.gen(function*() {
      const database = yield* Database

      const postColumns = {
        id: posts.id,
        body: posts.body,
        createdAt: posts.createdAt,
        author: users.username,
        likeCount: sql<number>`(SELECT count(*) FROM ${likes} WHERE ${likes.postId} = ${posts.id})`
      }

      const selectFeed = yield* database.execute((db) =>
        db
          .select(postColumns)
          .from(posts)
          .innerJoin(users, eq(users.id, posts.userId))
          .orderBy(desc(posts.createdAt), desc(posts.id))
          .limit(20)
          .prepare()
      )

      const selectPost = yield* database.execute((db) =>
        db
          .select(postColumns)
          .from(posts)
          .innerJoin(users, eq(users.id, posts.userId))
          .where(eq(posts.id, sql.placeholder("id")))
          .prepare()
      )

      const selectPostId = yield* database.execute((db) =>
        db
          .select({ id: posts.id })
          .from(posts)
          .where(eq(posts.id, sql.placeholder("postId")))
          .prepare()
      )

      const insertPostReturning = yield* database.execute((db) =>
        db
          .insert(posts)
          .values({ userId: sql.placeholder("userId"), body: sql.placeholder("body") })
          .returning({ id: posts.id, createdAt: posts.createdAt })
          .prepare()
      )

      const insertLikeReturning = yield* database.execute((db) =>
        db
          .insert(likes)
          .select(
            db
              .select({
                userId: sql<number>`${sql.placeholder("userId")}`.as("user_id"),
                postId: posts.id,
                createdAt: currentTimestamp.as("created_at")
              })
              .from(posts)
              .where(eq(posts.id, sql.placeholder("postId")))
          )
          .onConflictDoNothing()
          .returning({ postId: likes.postId })
          .prepare()
      )

      const findFeed = SqlSchema.findAll({
        Request: Schema.Void,
        Result: PostRow,
        execute: () => database.execute(() => selectFeed.all())
      })

      const findPost = SqlSchema.findOneOption({
        Request: Schema.Struct({ id: Schema.Int }),
        Result: PostRow,
        execute: (request) => database.execute(() => selectPost.all(request))
      })

      const findPostId = SqlSchema.findOneOption({
        Request: Schema.Struct({ postId: Schema.Int }),
        Result: Schema.Struct({ id: Schema.Int }),
        execute: (request) => database.execute(() => selectPostId.all(request))
      })

      const insertPostRow = SqlSchema.findOne({
        Request: NewPost,
        Result: CreatedPost,
        execute: (request) => database.execute(() => insertPostReturning.all(request))
      })

      const insertLikeRows = SqlSchema.findAll({
        Request: NewLike,
        Result: Schema.Struct({ postId: Schema.Int }),
        execute: (request) => database.execute(() => insertLikeReturning.all(request))
      })

      const commitBatch = <A>(writes: ReadonlyArray<Effect.Effect<A, PostRepoError>>) =>
        database.withTransaction(Effect.forEach(writes, Effect.exit)).pipe(
          Effect.mapError(repoError)
        )

      const applyPost = (request: InsertPost) =>
        insertPostRow({ userId: request.userId, body: request.body }).pipe(
          Effect.mapError(repoError)
        )

      const applyLike = Effect.fnUntraced(
        function*(request: InsertLike) {
          const inserted = yield* insertLikeRows({ userId: request.userId, postId: request.postId })

          if (Arr.isReadonlyArrayNonEmpty(inserted)) return "Liked" as const

          const existing = yield* findPostId({ postId: request.postId })

          return Option.match(existing, {
            onNone: () => "PostMissing" as const,
            onSome: () => "AlreadyLiked" as const
          })
        },
        Effect.mapError(repoError)
      )

      const postWrites = RequestResolver.make<InsertPost>((entries) =>
        commitBatch(entries.map((entry) => applyPost(entry.request))).pipe(
          Effect.flatMap((exits) =>
            Effect.forEach(Arr.zip(entries, exits), ([entry, exit]) => Request.complete(entry, exit), { discard: true })
          ),
          Effect.catch((error) => Effect.forEach(entries, (entry) => Request.fail(entry, error), { discard: true }))
        )
      ).pipe(
        RequestResolver.batchN(maxWritesPerCommit)
      )

      const likeWrites = RequestResolver.make<InsertLike>((entries) =>
        commitBatch(entries.map((entry) => applyLike(entry.request))).pipe(
          Effect.flatMap((exits) =>
            Effect.forEach(Arr.zip(entries, exits), ([entry, exit]) => Request.complete(entry, exit), { discard: true })
          ),
          Effect.catch((error) => Effect.forEach(entries, (entry) => Request.fail(entry, error), { discard: true }))
        )
      ).pipe(
        RequestResolver.batchN(maxWritesPerCommit)
      )

      const feed = findFeed(undefined).pipe(
        Effect.mapError(repoError),
        Effect.withSpan("PostRepo.feed")
      )

      const findById = Effect.fn("PostRepo.findById")((id: number) =>
        findPost({ id }).pipe(
          Effect.mapError(repoError)
        )
      )

      const insert = Effect.fn("PostRepo.insert")((input: { readonly userId: number; readonly body: string }) =>
        Effect.request(new InsertPost(input), postWrites)
      )

      const like = Effect.fn("PostRepo.like")((input: { readonly userId: number; readonly postId: number }) =>
        Effect.request(new InsertLike(input), likeWrites)
      )

      return PostRepo.of({ feed, findById, insert, like })
    })
  )
}
