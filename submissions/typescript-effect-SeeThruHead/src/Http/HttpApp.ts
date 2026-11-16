import { Effect, Layer, Schema } from "effect"
import { HttpRouter, HttpServerRespondable, HttpServerResponse } from "effect/http"
import { Auth } from "../Auth/Auth.ts"
import { HealthRoutes } from "../Health/HealthRoutes.ts"
import { PostRoutes } from "../Posts/PostRoutes.ts"
import { Posts } from "../Posts/Posts.ts"
import { errorResponse } from "./errorResponse.ts"

const routeNotFound = errorResponse(404, "not found")

export class RouteNotFound extends Schema.TaggedError<RouteNotFound>()("RouteNotFound", {}) {
  [HttpServerRespondable.symbol]() {
    return routeNotFound
  }
}

const NotFoundRoute = HttpRouter.add("*", "*", Effect.fail(new RouteNotFound()))

const logServerError = (response: HttpServerResponse.HttpServerResponse, error: unknown) => {
  if (response.status < 500) return Effect.void

  return Effect.logError("request failed", error)
}

const respondToDefect = (internalServerError: HttpServerResponse.HttpServerResponse) => (defect: unknown) => {
  if (HttpServerResponse.isHttpServerResponse(defect)) return Effect.succeed(defect)

  return Effect.logError("request defect", defect).pipe(
    Effect.as(internalServerError)
  )
}

const respondToFailures = (internalServerError: HttpServerResponse.HttpServerResponse) =>
<E, R>(app: Effect.Effect<HttpServerResponse.HttpServerResponse, E, R>) =>
  app.pipe(
    Effect.catch((error) =>
      HttpServerRespondable.toResponseOrElse(error, internalServerError).pipe(
        Effect.tap((response) => logServerError(response, error))
      )
    ),
    Effect.catchDefect(respondToDefect(internalServerError))
  )

export const layer = Layer.unwrap(
  Effect.gen(function*() {
    const internalServerError = yield* errorResponse(500, "internal server error")

    return HttpRouter.serve(
      Layer.mergeAll(PostRoutes, HealthRoutes, NotFoundRoute),
      { disableLogger: true, middleware: respondToFailures(internalServerError) }
    )
  })
).pipe(
  Layer.provide([Posts.layer, Auth.layer])
)
