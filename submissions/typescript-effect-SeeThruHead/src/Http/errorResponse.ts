import { Effect, Schema } from "effect"
import { HttpServerResponse } from "effect/http"

const ErrorBody = Schema.Struct({
  error: Schema.String
})

const encodeErrorBody = HttpServerResponse.schemaJson(ErrorBody)

export const errorResponse = (status: number, error: string) =>
  encodeErrorBody({ error }, { status }).pipe(
    Effect.orDie
  )
