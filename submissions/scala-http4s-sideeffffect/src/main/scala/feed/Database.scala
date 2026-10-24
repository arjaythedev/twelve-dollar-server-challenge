package feed

import cats.data.Kleisli
import cats.effect.{IO, Resource}
import doobie.*
import doobie.free.KleisliInterpreter
import doobie.implicits.*
import doobie.util.transactor.Strategy
import fs2.Stream
import org.sqlite.JDBC

import java.sql.Connection
import java.util.Properties
import scala.concurrent.duration.*

/** SQLite through doobie: a pool of read connections, a single write connection, and a background checkpointer.
  *
  * In WAL mode readers never wait for the writer, and with only one write connection, writes never wait on SQLite's
  * lock: they queue in the write pool as fibers instead.
  */
final class Database private (reader: Transactor[IO], writer: Transactor[IO]):
  def read[A](program: ConnectionIO[A]): IO[A] = program.transact(reader)
  def write[A](program: ConnectionIO[A]): IO[A] = program.transact(writer)

object Database:
  def resource(path: String): Resource[IO, Database] =
    val readers = math.max(2, Runtime.getRuntime.availableProcessors)
    for
      reader <- ConnectionPool(readers, open(path)).map(onComputePool)
      writer <- ConnectionPool(1, open(path)).map(onComputePool)
      _ <- checkpointer(path)
    yield Database(reader, writer)

  /** Settings for every connection. sqlite-jdbc applies them as PRAGMAs when it opens the connection. */
  private def properties: Properties =
    val p = Properties()
    // WAL with synchronous=NORMAL is the weakest durability rule 6 allows; a 201 still means the row is committed.
    p.setProperty("journal_mode", "WAL")
    p.setProperty("synchronous", "NORMAL")
    p.setProperty("busy_timeout", "5000")
    p.setProperty("cache_size", "-16384") // 16 MiB of page cache per connection, on top of mmap
    p.setProperty("mmap_size", "1073741824")
    p.setProperty("temp_store", "MEMORY")
    // No checkpoint inside a request: the background checkpointer does them (see below).
    p.setProperty("wal_autocheckpoint", "0")
    // Otherwise sqlite-jdbc runs `SELECT last_insert_rowid()` after every INSERT, for getGeneratedKeys.
    p.setProperty("jdbc.get_generated_keys", "false")
    p

  private def open(path: String): IO[Connection] =
    IO.blocking(StatementCachingConnection(JDBC.createConnection(s"jdbc:sqlite:$path", properties)))

  /** doobie runs every JDBC call as `blocking`, because a JDBC call usually waits on a network round trip. SQLite runs
    * in-process on data that is in the page cache (or `mmap`), so its calls are short CPU work: they run as plain
    * `delay` on the compute pool, without a hand-off to a blocking thread for every `next()` and `getString()`.
    */
  private val computeInterpreter: KleisliInterpreter[IO] =
    new KleisliInterpreter[IO](LogHandler.noop[IO]):
      override def primitive[J, A](f: J => A): Kleisli[IO, J, A] = Kleisli(j => IO.delay(f(j)))

  private def onComputePool(pool: ConnectionPool): Transactor[IO] =
    // Every request runs a single statement in autocommit mode, so there is no transaction to begin or end.
    Transactor[IO, ConnectionPool](pool, _.borrow, computeInterpreter.ConnectionInterpreter, Strategy.void)

  /** Checkpoints copy the WAL back into the database file and fsync it. With SQLite's automatic checkpoints, that
    * happens inside whichever write crosses 1,000 WAL pages, and on one vCPU its two fsyncs stall every request. Here a
    * background fiber runs a PASSIVE checkpoint every second, on its own connection, with doobie's default interpreter:
    * every call is `blocking`, so the fsyncs wait on a blocking thread. PASSIVE never waits for readers or the writer;
    * once it has copied everything, the next write starts the WAL from the beginning again.
    */
  private def checkpointer(path: String): Resource[IO, Unit] =
    val checkpoint = sql"PRAGMA wal_checkpoint(PASSIVE)".query[(Int, Int, Int)].unique
    for
      pool <- ConnectionPool(1, open(path))
      xa = Transactor[IO, ConnectionPool](
        pool,
        _.borrow,
        KleisliInterpreter[IO](LogHandler.noop[IO]).ConnectionInterpreter,
        Strategy.void
      )
      _ <- Stream.fixedDelay[IO](1.second).evalMap(_ => checkpoint.transact(xa).attempt).compile.drain.background
    yield ()
