package server

import com.sun.management.UnixOperatingSystemMXBean
import io.netty.channel.Channel
import io.netty.channel.ChannelDuplexHandler
import io.netty.channel.ChannelHandlerContext
import io.netty.channel.ChannelPromise
import io.netty.handler.codec.http.HttpRequest
import io.netty.handler.codec.http.LastHttpContent
import java.lang.management.ManagementFactory

/**
 * Keeps the server under its open-file limit the way nginx does when it runs out of connections:
 * once the open connections reach [max], each new connection closes the keep-alive connection that
 * has been idle the longest. A connection with a request in flight is never closed. Below [max]
 * idle connections stay open indefinitely.
 */
class IdleConnections(private val max: Int) {
    private val idle = LinkedHashSet<Channel>() // iteration order: idle the longest first
    private var open = 0

    @Synchronized
    private fun opened(ch: Channel) {
        open++
        idle.add(ch)
        if (open > max) {
            val it = idle.iterator()
            while (it.hasNext()) {
                val oldest = it.next()
                if (oldest === ch) continue
                it.remove()
                oldest.close()
                break
            }
        }
    }

    @Synchronized
    private fun closed(ch: Channel) {
        open--
        idle.remove(ch)
    }

    @Synchronized
    private fun busy(ch: Channel) {
        idle.remove(ch)
    }

    @Synchronized
    private fun idle(ch: Channel) {
        if (ch.isActive) idle.add(ch)
    }

    /** A handler for one connection's pipeline, placed after the HTTP codec. */
    fun handler(): ChannelDuplexHandler = object : ChannelDuplexHandler() {
        private var inFlight = 0
        private var counted = false

        // Ktor fires channelActive a second time from the codec once its HTTP/1 pipeline is set up.
        override fun channelActive(ctx: ChannelHandlerContext) {
            if (!counted) {
                counted = true
                opened(ctx.channel())
            }
            super.channelActive(ctx)
        }

        override fun channelInactive(ctx: ChannelHandlerContext) {
            if (counted) {
                counted = false
                closed(ctx.channel())
            }
            super.channelInactive(ctx)
        }

        override fun channelRead(ctx: ChannelHandlerContext, msg: Any) {
            if (msg is HttpRequest && inFlight++ == 0) busy(ctx.channel())
            super.channelRead(ctx, msg)
        }

        override fun write(ctx: ChannelHandlerContext, msg: Any, promise: ChannelPromise) {
            if (msg is LastHttpContent) {
                if (inFlight > 0) inFlight--
                if (inFlight == 0) idle(ctx.channel())
            }
            super.write(ctx, msg, promise)
        }
    }

    companion object {
        /** Room for the JVM's own files (jars, the database, epoll) under the process's open-file limit. */
        private const val RESERVED_FDS = 1024

        fun forFileLimit(): IdleConnections {
            val limit = (ManagementFactory.getOperatingSystemMXBean() as UnixOperatingSystemMXBean).maxFileDescriptorCount
            return IdleConnections(maxOf(limit - RESERVED_FDS, 64L).coerceAtMost(Int.MAX_VALUE.toLong()).toInt())
        }
    }
}
