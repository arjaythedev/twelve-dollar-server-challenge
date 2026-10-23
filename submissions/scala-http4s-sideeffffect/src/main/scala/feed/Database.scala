package feed

import cats.data.Kleisli
import cats.effect.std.Mutex
import cats.effect.{IO, Resource}
import com.zaxxer.hikari.HikariConfig
import doobie.*
import doobie.free.KleisliInterpreter
import doobie.hikari.HikariTransactor
import doobie.implicits.*
import doobie.util.transactor.Strategy

/** SQLite through doobie: a pool of read connections and a single write connection.
  *
  * In WAL mode readers never wait for the writer, and with only one writer, writes never wait on SQLite's lock: they
  * are queued semantically on a [[Mutex]] instead.
  */
final class Database private (reader: Transactor[IO], writer: Transactor[IO], writeLock: Mutex[IO]):
  def read[A](program: ConnectionIO[A]): IO[A] = program.transact(reader)
  def write[A](program: ConnectionIO[A]): IO[A] = writeLock.lock.surround(program.transact(writer))

object Database:
  def resource(path: String): Resource[IO, Database] =
    val readers = math.max(2, Runtime.getRuntime.availableProcessors)
    for
      ec <- Resource.eval(IO.executionContext)
      reader <- transactor(path, "reader", readers, ec)
      writer <- transactor(path, "writer", 1, ec)
      writeLock <- Resource.eval(Mutex[IO])
    yield Database(reader, writer, writeLock)

  /** Doobie runs every JDBC call as `blocking`, because a JDBC call usually waits on a network round trip. SQLite runs
    * in-process on data that is in the page cache (or `mmap`), so its calls are short CPU work: they run as plain
    * `delay` on the compute pool, without a hand-off to a blocking thread for every `next()` and `getString()`.
    */
  private def interpreter: KleisliInterpreter[IO] =
    new KleisliInterpreter[IO](LogHandler.noop[IO]):
      override def primitive[J, A](f: J => A): Kleisli[IO, J, A] = Kleisli(j => IO.delay(f(j)))

  private def transactor(
      path: String,
      name: String,
      poolSize: Int,
      ec: scala.concurrent.ExecutionContext
  ): Resource[IO, Transactor[IO]] =
    val config = HikariConfig()
    config.setPoolName(name)
    config.setJdbcUrl(s"jdbc:sqlite:$path")
    config.setMaximumPoolSize(poolSize)
    config.setMinimumIdle(poolSize)
    config.setAutoCommit(true)
    // Read by sqlite-jdbc and applied as PRAGMAs on every new connection. WAL with synchronous=NORMAL is the
    // weakest durability rule 6 allows; a 201 still means the row is committed.
    config.addDataSourceProperty("journal_mode", "WAL")
    config.addDataSourceProperty("synchronous", "NORMAL")
    config.addDataSourceProperty("busy_timeout", "5000")
    config.addDataSourceProperty("cache_size", "-65536") // 64 MiB of page cache per connection
    config.addDataSourceProperty("mmap_size", "1073741824")
    config.addDataSourceProperty("temp_store", "MEMORY")
    HikariTransactor
      .fromHikariConfigCustomEc[IO](config, ec)
      .map(
        // Every request runs a single statement in autocommit mode, so there is no transaction to begin or end.
        _.copy(interpret0 = interpreter.ConnectionInterpreter, strategy0 = Strategy.void)
      )
