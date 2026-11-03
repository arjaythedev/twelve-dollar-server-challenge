package feed

import cats.effect.IO

import java.lang.foreign.ValueLayout.{ADDRESS, JAVA_BYTE, JAVA_INT}
import java.lang.foreign.{Arena, FunctionDescriptor, Linker}
import java.nio.file.{Files, Path}
import scala.jdk.CollectionConverters.*
import scala.util.Using

/** fs2 binds the server socket with Java's default listen backlog of 50, and neither fs2 nor Ember has a setting for
  * it. When thousands of users connect at once, the kernel's accept queue overflows and the dropped handshakes come
  * back as multi-second retransmit delays. On Linux, calling `listen()` again on a listening socket changes its
  * backlog, so this finds the socket listening on `port` among the process's file descriptors and does that (the kernel
  * caps the value at `net.core.somaxconn`).
  */
object ListenBacklog:
  private val SolSocket = 1
  private val SoAcceptConn = 30

  def raise(port: Int, backlog: Int): IO[Boolean] = IO
    .blocking {
      val linker = Linker.nativeLinker
      val libc = linker.defaultLookup
      def function(name: String, descriptor: FunctionDescriptor) =
        linker.downcallHandle(libc.find(name).get, descriptor)
      val listen = function("listen", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT))
      val getsockopt =
        function("getsockopt", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS))
      val getsockname = function("getsockname", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS))

      Using.resource(Arena.ofConfined) { arena =>
        val value = arena.allocate(JAVA_INT)
        val length = arena.allocate(JAVA_INT)
        val address = arena.allocate(128)

        def isListening(fd: Int): Boolean =
          length.set(JAVA_INT, 0, 4)
          val rc = getsockopt.invokeWithArguments(fd, SolSocket, SoAcceptConn, value, length).asInstanceOf[Int]
          rc == 0 && value.get(JAVA_INT, 0) == 1

        // sockaddr_in and sockaddr_in6 both keep the port, big-endian, at offset 2.
        def boundPort(fd: Int): Int =
          length.set(JAVA_INT, 0, 128)
          val rc = getsockname.invokeWithArguments(fd, address, length).asInstanceOf[Int]
          if rc != 0 then -1
          else
            ((address.get(JAVA_BYTE, 2) & 0xff) << 8) |
              (address.get(JAVA_BYTE, 3) & 0xff)

        val fds = Using
          .resource(Files.list(Path.of("/proc/self/fd")))(_.iterator.asScala.toList)
          .flatMap(_.getFileName.toString.toIntOption)
        fds
          .filter(fd => isListening(fd) && boundPort(fd) == port)
          .map(fd => listen.invokeWithArguments(fd, backlog).asInstanceOf[Int] == 0)
          .exists(identity)
      }
    }
    .handleError(_ => false)
