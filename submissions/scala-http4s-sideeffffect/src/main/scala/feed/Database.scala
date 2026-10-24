package feed

import cats.data.Kleisli
import cats.effect.std.{Console, Queue}
import cats.effect.{Deferred, IO, Ref, Resource}
import cats.syntax.all.*
import doobie.*
import doobie.free.KleisliInterpreter
import doobie.implicits.*
import doobie.util.transactor.Strategy
import fs2.{Chunk, Stream}
import org.sqlite.JDBC

import java.sql.Connection
import java.util.Properties
import java.util.concurrent.Executors
import scala.concurrent.ExecutionContext
import scala.concurrent.duration.*

/** SQLite through doobie: a pool of read connections, one write connection behind a group commit, and a background
  * checkpointer.
  *
  * In WAL mode readers never wait for the writer. Writes go through [[Database.groupCommit]]: each request's write runs
  * in the next group's transaction, and the request is answered only after that transaction commits (rule 6).
  */
final class Database private (reader: Transactor[IO], writes: Queue[IO, Database.PendingWrite[?]]):
  def read[A](program: ConnectionIO[A]): IO[A] = program.transact(reader)

  /** Runs `program` in the next group commit and completes once that commit is on disk. */
  def write[A](program: ConnectionIO[A]): IO[A] =
    Deferred[IO, Either[Throwable, A]].flatMap(result =>
      writes.offer(Database.PendingWrite(program, result)) *> result.get.rethrow
    )

object Database:
  def resource(path: String): Resource[IO, Database] =
    val readers = math.max(2, Runtime.getRuntime.availableProcessors)
    for
      reader <- ConnectionPool(readers, open(path).map(StatementCachingConnection(_)))
      writer <- ConnectionPool(1, open(path).map(StatementCachingConnection(_)))
      walPages <- Resource.eval(Ref[IO].of(0))
      writes <- groupCommit(writer, walPages)
      _ <- checkpointer(path, walPages)
    yield Database(onComputePool(reader), writes)

  /** A write waiting for its group commit. `run` executes the statement inside the group's transaction and returns what
    * answers the caller, which happens only after the commit.
    */
  final case class PendingWrite[A](program: ConnectionIO[A], result: Deferred[IO, Either[Throwable, A]]):
    def run: ConnectionIO[IO[Unit]] = program.attempt.map(outcome => result.complete(outcome).void)
    def fail(error: Throwable): IO[Unit] = result.complete(Left(error)).void

  /** Group commit: one stream takes whatever writes are queued, runs them in one transaction on the write connection,
    * commits, and only then answers each request. There is no timer: an idle server commits a group of one at once;
    * under load, the writes that arrive while one group commits form the next group.
    *
    * A failing statement doesn't abort an SQLite transaction, so each write's own error goes to its own caller; only a
    * failed commit fails the whole group. Disk work (a commit, a WAL restart) must not hold the compute thread, but a
    * thread hand-off per JDBC call (doobie's default interpreter) costs more than the writes themselves on one vCPU. So
    * the whole group runs, call by call as plain `delay`, on a dedicated writer thread: one hand-off per group.
    *
    * Between two groups is also the one moment no write is in flight, so that is where the WAL gets restarted when it
    * has grown (see [[checkpointer]]).
    */
  private def groupCommit(writer: ConnectionPool, walPages: Ref[IO, Int]): Resource[IO, Queue[IO, PendingWrite[?]]] =
    val xa =
      Transactor[IO, ConnectionPool](writer, _.borrow, computeInterpreter.ConnectionInterpreter, Strategy.default)
    val outsideTransaction = Transactor.strategy.set(xa, Strategy.void)
    val restartWal = sql"PRAGMA wal_checkpoint(RESTART)".query[(Int, Int, Int)].unique.transact(outsideTransaction)
    for
      thread <- Resource
        .make(IO(Executors.newSingleThreadExecutor(Thread.ofPlatform.name("sqlite-writer").factory)))(pool =>
          IO.blocking(pool.shutdown())
        )
        .map(ExecutionContext.fromExecutorService)
      commit = (group: Chunk[PendingWrite[?]]) =>
        group.traverse(_.run).transact(xa).evalOn(thread).attempt.flatMap {
          case Right(answers) => answers.sequence_
          case Left(error)    => group.traverse_(_.fail(error))
        }
      restartIfLarge = walPages.get.flatMap(pages =>
        IO.whenA(pages > RestartWalAt)(restartWal.evalOn(thread).attempt *> walPages.set(0))
      )
      queue <- Resource.eval(Queue.unbounded[IO, PendingWrite[?]])
      _ <- Stream
        .fromQueueUnterminated(queue, 256)
        .chunks
        .evalMap(group => commit(group) *> restartIfLarge)
        .compile
        .drain
        .background
    yield queue

  /** Settings for every connection. sqlite-jdbc applies them as PRAGMAs when it opens the connection. */
  private def properties: Properties =
    val p = Properties()
    // WAL with synchronous=NORMAL, the setting rule 6 asks for at least: a commit survives the process crashing, but
    // not a power loss. FULL (an fsync per commit) cost p99 4.3 s at 32,000 users even with group commit (FINDINGS.md).
    p.setProperty("journal_mode", "WAL")
    p.setProperty("synchronous", "NORMAL")
    // With WAL, one writer and PASSIVE checkpoints nothing should wait for a lock; if something ever does, it holds
    // a thread, so give up after a second rather than five.
    p.setProperty("busy_timeout", "1000")
    p.setProperty("cache_size", "-16384") // 16 MiB of page cache per connection, on top of mmap
    p.setProperty("mmap_size", "1073741824")
    p.setProperty("temp_store", "MEMORY")
    // Checkpoints run in the background (see below). SQLite's own, inside a request, only as a backstop: at 10,000
    // pages (~40 MB of WAL) instead of 1,000, in case the background checkpointer falls behind or fails.
    p.setProperty("wal_autocheckpoint", "10000")
    // Otherwise sqlite-jdbc runs `SELECT last_insert_rowid()` after every INSERT, for getGeneratedKeys.
    p.setProperty("jdbc.get_generated_keys", "false")
    p

  private def open(path: String): IO[Connection] =
    IO.blocking(JDBC.createConnection(s"jdbc:sqlite:$path", properties))

  /** doobie runs every JDBC call as `blocking`, because a JDBC call usually waits on a network round trip. SQLite runs
    * in-process on data that is in the page cache (or `mmap`), so its reads are short CPU work: they run as plain
    * `delay` on the compute pool, without a hand-off to a blocking thread for every `next()` and `getString()`.
    */
  private val computeInterpreter: KleisliInterpreter[IO] =
    new KleisliInterpreter[IO](LogHandler.noop[IO]):
      override def primitive[J, A](f: J => A): Kleisli[IO, J, A] = Kleisli(j => IO.delay(f(j)))

  private def onComputePool(pool: ConnectionPool): Transactor[IO] =
    // Every read is a single statement in autocommit mode, so there is no transaction to begin or end.
    Transactor[IO, ConnectionPool](pool, _.borrow, computeInterpreter.ConnectionInterpreter, Strategy.void)

  /** Checkpoints copy the WAL back into the database file and fsync it. With SQLite's automatic checkpoints, that
    * happens inside whichever write crosses 1,000 WAL pages. Here a background fiber runs a PASSIVE checkpoint every
    * second, on its own connection, with doobie's default interpreter: every call is `blocking`, so the fsyncs wait on
    * a blocking thread. PASSIVE never waits for readers or the writer.
    *
    * PASSIVE alone isn't enough on a real disk: with reads arriving all the time, some reader always holds a snapshot
    * that still needs the WAL, so the WAL is never restarted and keeps growing until SQLite's own checkpoint fires
    * inside a request. So the checkpointer reports the WAL's size, and once it passes [[RestartWalAt]] pages the writer
    * runs a RESTART checkpoint between two groups: it waits for the readers of the moment (milliseconds), and the next
    * write starts the WAL from the beginning. A failure is reported and retried; the 10,000-page autocheckpoint is the
    * backstop.
    */
  private val RestartWalAt = 4000

  private def checkpointer(path: String, walPages: Ref[IO, Int]): Resource[IO, Unit] =
    val checkpoint = sql"PRAGMA wal_checkpoint(PASSIVE)".query[(Int, Int, Int)].unique
    for
      pool <- ConnectionPool(1, open(path))
      xa = Transactor[IO, ConnectionPool](
        pool,
        _.borrow,
        KleisliInterpreter[IO](LogHandler.noop[IO]).ConnectionInterpreter,
        Strategy.void
      )
      report = (e: Throwable) => Console[IO].errorln(s"checkpoint failed: $e")
      _ <- Stream
        .fixedDelay[IO](1.second)
        .evalMap(_ => checkpoint.transact(xa).flatMap((_, pages, _) => walPages.set(pages)).handleErrorWith(report))
        .compile
        .drain
        .background
    yield ()
