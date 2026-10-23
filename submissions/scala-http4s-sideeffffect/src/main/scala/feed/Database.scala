package feed

import cats.data.Kleisli
import cats.effect.std.{Mutex, Semaphore}
import cats.effect.{IO, Resource}
import com.zaxxer.hikari.{HikariConfig, HikariDataSource}
import doobie.*
import doobie.free.KleisliInterpreter
import doobie.implicits.*
import doobie.util.transactor.Strategy

/** SQLite through doobie: a pool of read connections and a single write connection.
  *
  * In WAL mode readers never wait for the writer, and with only one writer, writes never wait on SQLite's lock.
  * Requests wait for a connection on a semaphore, as fibers. They never get as far as Hikari's `getConnection` while
  * the pool is empty: that call would block the thread, and on one vCPU that is the thread the connection holders need
  * to finish and give their connection back.
  */
final class Database private (
    reader: Transactor[IO],
    readers: Semaphore[IO],
    writer: Transactor[IO],
    writeLock: Mutex[IO]
):
  def read[A](program: ConnectionIO[A]): IO[A] = readers.permit.surround(program.transact(reader))
  def write[A](program: ConnectionIO[A]): IO[A] = writeLock.lock.surround(program.transact(writer))

object Database:
  def resource(path: String): Resource[IO, Database] =
    val readPoolSize = math.max(2, Runtime.getRuntime.availableProcessors)
    for
      reader <- transactor(path, "reader", readPoolSize)
      readers <- Resource.eval(Semaphore[IO](readPoolSize))
      writer <- transactor(path, "writer", 1)
      writeLock <- Resource.eval(Mutex[IO])
    yield Database(reader, readers, writer, writeLock)

  /** Doobie runs every JDBC call as `blocking`, because a JDBC call usually waits on a network round trip. SQLite runs
    * in-process on data that is in the page cache (or `mmap`), so its calls are short CPU work: they run as plain
    * `delay` on the compute pool, without a hand-off to a blocking thread for every `next()` and `getString()`. The
    * same goes for taking a connection from the pool and giving it back (the semaphores make sure one is idle).
    */
  private def interpreter: KleisliInterpreter[IO] =
    new KleisliInterpreter[IO](LogHandler.noop[IO]):
      override def primitive[J, A](f: J => A): Kleisli[IO, J, A] = Kleisli(j => IO.delay(f(j)))

  private def transactor(
      path: String,
      name: String,
      poolSize: Int
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
    config.addDataSourceProperty("cache_size", "-16384") // 16 MiB of page cache per connection, on top of mmap
    config.addDataSourceProperty("mmap_size", "1073741824")
    config.addDataSourceProperty("temp_store", "MEMORY")
    Resource
      .fromAutoCloseable(IO.blocking(HikariDataSource(config)))
      .map(dataSource =>
        Transactor[IO, HikariDataSource](
          dataSource,
          ds => Resource.make(IO.delay(ds.getConnection))(connection => IO.delay(connection.close())),
          interpreter.ConnectionInterpreter,
          // Every request runs a single statement in autocommit mode, so there is no transaction to begin or end.
          Strategy.void
        )
      )
