import { BunHttpServer, BunRuntime } from "@effect/platform-bun"
import { Config, Layer } from "effect"
import * as BunSqlite from "./Database/BunSqlite.ts"
import * as HttpApp from "./Http/HttpApp.ts"

const ServerLive = BunHttpServer.layerConfig({
  hostname: Config.String("HOST"),
  port: Config.Port("PORT"),
  idleTimeout: Config.succeed(75)
})

HttpApp.layer.pipe(
  Layer.provide([ServerLive, BunSqlite.layer]),
  Layer.launch,
  BunRuntime.runMain
)
