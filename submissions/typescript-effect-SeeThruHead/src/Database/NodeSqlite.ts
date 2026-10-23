import BetterSqlite3 from "better-sqlite3"
import { drizzle } from "drizzle-orm/better-sqlite3"
import { Config, Effect, Layer } from "effect"
import { Database, databaseError } from "./Database.ts"

export const layer = Layer.effect(
  Database,
  Effect.gen(function*() {
    const filename = yield* Config.NonEmptyString("SQLITE_PATH")

    const client = yield* Effect.acquireRelease(
      Effect.try({
        try: () => new BetterSqlite3(filename, { fileMustExist: true }),
        catch: databaseError
      }),
      (client) => Effect.sync(() => client.close())
    )

    return yield* Database.make(drizzle({ client }))
  })
)
