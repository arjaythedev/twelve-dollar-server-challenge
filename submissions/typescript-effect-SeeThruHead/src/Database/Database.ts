import { sql } from "drizzle-orm"
import type { BaseSQLiteDatabase } from "drizzle-orm/sqlite-core"
import { Context, Effect, Exit, Predicate, Schema, Semaphore } from "effect"

export class DatabaseError extends Schema.TaggedError<DatabaseError>()("DatabaseError", {
  message: Schema.String,
  cause: Schema.Defect()
}) {}

export const databaseError = (cause: unknown) =>
  new DatabaseError({
    message: Predicate.isError(cause) ? cause.message : "unknown database failure",
    cause
  })

const InTransaction = Context.Reference<boolean>("tdsc/Database/InTransaction", {
  defaultValue: () => false
})

const pragmas = [
  sql`PRAGMA journal_mode = WAL`,
  sql`PRAGMA synchronous = NORMAL`,
  sql`PRAGMA busy_timeout = 5000`,
  sql`PRAGMA cache_size = -65536`,
  sql`PRAGMA mmap_size = 536870912`,
  sql`PRAGMA temp_store = MEMORY`
]

const begin = sql`BEGIN IMMEDIATE`
const commit = sql`COMMIT`
const rollback = sql`ROLLBACK`

type SyncDatabase = BaseSQLiteDatabase<"sync", unknown>

export class Database extends Context.Service<Database, {
  execute<A>(statement: (database: SyncDatabase) => A): Effect.Effect<A, DatabaseError>
  withTransaction<A, E, R>(effect: Effect.Effect<A, E, R>): Effect.Effect<A, E | DatabaseError, R>
}>()("tdsc/Database/Database") {
  static readonly make = Effect.fnUntraced(function*(database: SyncDatabase) {
    const connection = yield* Semaphore.make(1)

    const run = <A>(statement: (database: SyncDatabase) => A) =>
      Effect.try({
        try: () => statement(database),
        catch: databaseError
      })

    const rollbackQuietly = run((db) => db.run(rollback)).pipe(
      Effect.ignore({ log: true, message: "Database.rollback failed" })
    )

    yield* Effect.forEach(pragmas, (pragma) => run((db) => db.run(pragma)), { discard: true })

    const execute = Effect.fnUntraced(function*<A>(statement: (database: SyncDatabase) => A) {
      const inTransaction = yield* InTransaction

      if (inTransaction) return yield* run(statement)

      return yield* connection.withPermit(run(statement))
    })

    const transact = <A, E, R>(effect: Effect.Effect<A, E, R>) =>
      Effect.uninterruptibleMask((restore) =>
        Effect.gen(function*() {
          yield* run((db) => db.run(begin))

          const exit = yield* Effect.exit(restore(Effect.provideService(effect, InTransaction, true)))

          if (Exit.isFailure(exit)) {
            yield* rollbackQuietly

            return yield* exit
          }

          yield* run((db) => db.run(commit)).pipe(
            Effect.tapError(() => rollbackQuietly)
          )

          return exit.value
        })
      )

    const withTransaction = Effect.fnUntraced(function*<A, E, R>(effect: Effect.Effect<A, E, R>) {
      const inTransaction = yield* InTransaction

      if (inTransaction) return yield* effect

      return yield* connection.withPermit(transact(effect))
    })

    return Database.of({ execute, withTransaction })
  })
}
