import { NodeHttpServer, NodeRuntime } from "@effect/platform-node"
import { Config, Layer } from "effect"
import { createServer } from "node:http"
import * as NodeSqlite from "./Database/NodeSqlite.ts"
import * as HttpApp from "./Http/HttpApp.ts"

const ServerLive = NodeHttpServer.layerConfig(
  () => createServer({ keepAliveTimeout: 75_000, headersTimeout: 80_000 }),
  {
    host: Config.String("HOST"),
    port: Config.Port("PORT"),
    backlog: Config.succeed(4096)
  }
)

HttpApp.layer.pipe(
  Layer.provide([ServerLive, NodeSqlite.layer]),
  Layer.launch,
  NodeRuntime.runMain
)
