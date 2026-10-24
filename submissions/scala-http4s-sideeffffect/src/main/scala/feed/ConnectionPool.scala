package feed

import cats.effect.std.Queue
import cats.effect.{IO, Resource}
import cats.syntax.all.*

import java.sql.Connection

/** A fixed set of connections in a queue. Borrowing takes one out and returning puts it back, so a request that finds
  * the pool empty waits as a fiber, never by blocking a thread. Each connection stays the same physical connection for
  * the life of the process, which is what lets it keep its prepared statements.
  */
final class ConnectionPool private (connections: Queue[IO, Connection]):
  val borrow: Resource[IO, Connection] = Resource.make(connections.take)(connections.offer)

object ConnectionPool:
  def apply(size: Int, open: IO[Connection]): Resource[IO, ConnectionPool] =
    for
      queue <- Resource.eval(Queue.bounded[IO, Connection](size))
      opened <- List.fill(size)(Resource.make(open)(c => IO.blocking(c.close()))).sequence
      _ <- Resource.eval(opened.traverse_(queue.offer))
    yield new ConnectionPool(queue)
