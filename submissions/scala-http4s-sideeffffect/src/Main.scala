package feed

import cats.effect.std.Console
import cats.effect.unsafe.IORuntimeConfig
import cats.effect.{IO, IOApp, Resource}
import org.http4s.ember.server.EmberServerBuilder

import scala.concurrent.duration.*

object Main extends IOApp.Simple:

  /** One worker thread per CPU. Cats Effect's default is at least 2, and on one vCPU the two workers spend most of
    * their time waking each other up to steal work that one of them could just run.
    */
  override protected def computeWorkerThreadCount: Int = Runtime.getRuntime.availableProcessors

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
        // Behind Nginx: it closes idle upstream connections after 60 s, so never close them first (SPEC: >= 65 s).
        .withIdleTimeout(Duration.Inf)
        // Nginx opens a new upstream connection for every request in flight beyond its 64 idle ones.
        .withMaxConnections(16384)
        // Ember reads into a 256 KiB buffer per connection by default. Requests here are a few hundred bytes.
        .withReceiveBufferSize(8 * 1024)
        .withShutdownTimeout(1.second)
        .build
      raised <- Resource.eval(ListenBacklog.raise(config.port.value, 65535))
      _ <- Resource.eval(IO.unlessA(raised)(Console[IO].errorln("could not raise the listen backlog")))
    yield server
    server.useForever
