require "./sys"
require "./buf"
require "./json"
require "./jwt"
require "./http"
require "./sqlite"
require "./db"
require "./net"
require "./uring"
require "./app"

secret = Sys.env("JWT_SECRET") || Sys.die("JWT_SECRET is required")
path = Sys.env("SQLITE_PATH") || Sys.die("SQLITE_PATH is required")
Jwt.init(secret)
Db.open(path)
Server.run(Sys.env("HOST") || "0.0.0.0", (Sys.env("PORT") || "3000").to_i)
