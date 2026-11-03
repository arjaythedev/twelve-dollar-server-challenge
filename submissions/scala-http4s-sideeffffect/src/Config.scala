package feed

import cats.effect.IO
import com.comcast.ip4s.{Host, Port}

/** The runtime contract from SPEC.md: everything comes from environment variables. */
final case class Config(sqlitePath: String, jwtSecret: String, host: Host, port: Port)

object Config:
  def load: IO[Config] = IO(sys.env).flatMap { env =>
    def required(name: String): IO[String] =
      IO.fromOption(env.get(name).filter(_.nonEmpty))(IllegalArgumentException(s"$name is not set"))

    for
      sqlitePath <- required("SQLITE_PATH")
      jwtSecret <- required("JWT_SECRET")
      host <- IO.fromOption(Host.fromString(env.getOrElse("HOST", "0.0.0.0")))(IllegalArgumentException("bad HOST"))
      port <- IO.fromOption(env.get("PORT").orElse(Some("3000")).flatMap(Port.fromString))(
        IllegalArgumentException("bad PORT")
      )
    yield Config(sqlitePath, jwtSecret, host, port)
  }
