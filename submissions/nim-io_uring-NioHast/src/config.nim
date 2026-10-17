## Environment configuration (env-only, per SPEC).
import std/[os, strutils]

type Config* = object
  sqlitePath*: string
  jwtSecret*: string
  host*: string
  port*: int

proc loadConfig*(): Config =
  result.sqlitePath = getEnv("SQLITE_PATH", "seed/feed.db")
  result.jwtSecret = getEnv("JWT_SECRET", "twelve-dollar-challenge")
  result.host = getEnv("HOST", "127.0.0.1")
  var port = 3000
  try:
    port = parseInt(getEnv("PORT", "3000"))
  except ValueError:
    port = 3000
  result.port = port
