import { sql } from "drizzle-orm"
import { Clock, Duration, Effect, Schema } from "effect"
import { HttpRouter, HttpServerRespondable, HttpServerResponse } from "effect/http"
import { Database } from "../Database/Database.ts"

const DegradedResponse = Schema.Struct({
  status: Schema.Literal("degraded"),
  db: Schema.Literal("unreachable"),
  error: Schema.String
})

const encodeDegraded = HttpServerResponse.schemaJson(DegradedResponse)

export class DatabaseUnreachable extends Schema.TaggedError<DatabaseUnreachable>()("DatabaseUnreachable", {
  message: Schema.String
}) {
  [HttpServerRespondable.symbol]() {
    return encodeDegraded({ status: "degraded", db: "unreachable", error: this.message }, { status: 503 }).pipe(
      Effect.orDie
    )
  }
}

const HealthyResponse = Schema.Struct({
  status: Schema.Literal("ok"),
  db: Schema.Literal("ok"),
  uptimeSeconds: Schema.Int
}).pipe(
  Schema.encodeKeys({ uptimeSeconds: "uptime_s" })
)

const encodeHealthy = HttpServerResponse.schemaJson(HealthyResponse)

export const HealthRoutes = HttpRouter.use(Effect.fn(function*(router) {
  const database = yield* Database
  const startedAt = yield* Clock.currentTimeMillis

  const uptime = Clock.currentTimeMillis.pipe(
    Effect.map((now) => Math.floor(Duration.toSeconds(Duration.millis(now - startedAt))))
  )

  yield* router.add("GET", "/health", Effect.gen(function*() {
    yield* database.execute((db) => db.get(sql`SELECT 1`)).pipe(
      Effect.mapError((error) => new DatabaseUnreachable({ message: error.message }))
    )

    const uptimeSeconds = yield* uptime

    return yield* encodeHealthy({ status: "ok", db: "ok", uptimeSeconds }).pipe(Effect.orDie)
  }))
}))
