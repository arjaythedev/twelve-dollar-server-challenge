package feed

import cats.effect.unsafe.IORuntimeConfig
import cats.effect.{IO, IOApp, Resource}
import org.http4s.ember.server.EmberServerBuilder

import scala.concurrent.duration.*

object Main extends IOApp.Simple:

  // Queries run on the compute pool (see Database), so a busy moment can look like starvation to the checker.
  override protected def runtimeConfig: IORuntimeConfig =
    super.runtimeConfig.copy(cpuStarvationCheckInitialDelay = Duration.Inf)

  def run: IO[Unit] =
    val server = for
      config <- Resource.eval(Config.load)
      startedAt <- Resource.eval(IO.monotonic)
      db <- Database.resource(config.sqlitePath)
      routes = Routes(db, Jwt(config.jwtSecret), startedAt)
      server <- EmberServerBuilder
        .default[IO]
        .withHost(config.host)
        .withPort(config.port)
        .withHttpApp(routes.httpApp)
        .withErrorHandler(_ => IO.pure(Routes.internalError))
        // Direct: every load-test user holds its own keep-alive connection, idle between requests for up to ~30 s.
        // Keep them open until the client closes them, and allow as many as the 65,535 descriptors leave room for.
        .withIdleTimeout(Duration.Inf)
        .withMaxConnections(64000)
        .withShutdownTimeout(1.second)
        .build
    yield server
    server.useForever
