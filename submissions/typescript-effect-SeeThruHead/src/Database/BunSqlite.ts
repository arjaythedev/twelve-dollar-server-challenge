import { Database as BunDatabase } from "bun:sqlite"
import { drizzle } from "drizzle-orm/bun-sqlite"
import { Config, Effect, Layer } from "effect"
import { Database, databaseError } from "./Database.ts"

export const layer = Layer.effect(
  Database,
  Effect.gen(function*() {
    const filename = yield* Config.NonEmptyString("SQLITE_PATH")

    const client = yield* Effect.acquireRelease(
      Effect.try({
        try: () => new BunDatabase(filename, { readwrite: true, create: false, strict: true }),
        catch: databaseError
      }),
      (client) => Effect.sync(() => client.close())
    )

    return yield* Database.make(drizzle({ client }))
  })
)
