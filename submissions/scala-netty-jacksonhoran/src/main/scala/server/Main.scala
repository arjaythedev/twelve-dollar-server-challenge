package server

import io.netty.bootstrap.ServerBootstrap
import io.netty.channel.{Channel, ChannelInitializer, ChannelOption, IoHandlerFactory, MultiThreadIoEventLoopGroup}
import io.netty.channel.epoll.{Epoll, EpollIoHandler, EpollServerSocketChannel}
import io.netty.channel.nio.NioIoHandler
import io.netty.channel.socket.ServerSocketChannel
import io.netty.channel.socket.nio.NioServerSocketChannel
import java.util.concurrent.TimeUnit

object Main:
  def main(args: Array[String]): Unit =
    def env(k: String, default: String | Null = null): String =
      val v = System.getenv(k)
      if v != null && v.nonEmpty then v
      else if default != null then default
      else { System.err.println(s"missing env var $k"); sys.exit(1) }

    val db = Sqlite.open(env("SQLITE_PATH"))
    db.execute("PRAGMA journal_mode = WAL")
    db.execute("PRAGMA synchronous = NORMAL") // rule 6: a commit is in the WAL before we respond
    db.execute("PRAGMA mmap_size = 1073741824")
    db.execute("PRAGMA cache_size = -65536")
    db.execute("PRAGMA temp_store = MEMORY")
    val app = App(db, Jwt(env("JWT_SECRET")))

    // One thread does everything (accept, parse, SQLite, write): the box has one vCPU, and one
    // connection used from one thread never waits on a lock.
    val epoll = Epoll.isAvailable
    val factory: IoHandlerFactory = if epoll then EpollIoHandler.newFactory() else NioIoHandler.newFactory()
    val loop = MultiThreadIoEventLoopGroup(1, factory)
    val channelClass: Class[? <: ServerSocketChannel] =
      if epoll then classOf[EpollServerSocketChannel] else classOf[NioServerSocketChannel]

    val server = ServerBootstrap()
      .group(loop)
      .channel(channelClass)
      .option(ChannelOption.SO_BACKLOG, Integer.valueOf(65535))
      .option(ChannelOption.SO_REUSEADDR, java.lang.Boolean.TRUE)
      .childOption(ChannelOption.TCP_NODELAY, java.lang.Boolean.TRUE)
      .childHandler(new ChannelInitializer[Channel]:
        override def initChannel(ch: Channel): Unit = ch.pipeline().addLast(Http.Handler(app)))
      .bind(env("HOST", "0.0.0.0"), env("PORT", "3000").toInt)
      .sync()
      .channel()
    loop.scheduleAtFixedRate(() => Http.tickDate(), 1, 1, TimeUnit.SECONDS)
    System.err.println(s"listening on ${server.localAddress()} (${if epoll then "epoll" else "nio"}); ${Http.reapThresholds}")
    server.closeFuture().sync()
