package borg.trikeshed.feed

import java.io.IOException
import java.net.InetSocketAddress
import java.net.StandardSocketOptions
import java.nio.ByteBuffer
import java.nio.channels.SelectionKey
import java.nio.channels.Selector
import java.nio.channels.ServerSocketChannel
import java.nio.channels.SocketChannel
import java.util.function.Consumer

actual class Handle(val channel: SocketChannel) {
    var key: SelectionKey? = null
}

/** java.nio: one Selector (epoll on Linux); bytes cross between Kotlin arrays and two direct buffers. */
actual class Poller actual constructor(host: String, port: Int) {
    val selector: Selector = Selector.open()
    val server: ServerSocketChannel = ServerSocketChannel.open().apply {
        setOption(StandardSocketOptions.SO_REUSEADDR, true)
        bind(InetSocketAddress(host, port), 65535)
        configureBlocking(false)
        register(selector, SelectionKey.OP_ACCEPT)
    }
    val rx: ByteBuffer = ByteBuffer.allocateDirect(1 shl 16)
    val tx: ByteBuffer = ByteBuffer.allocateDirect(1 shl 18)
    var xio: Xio? = null
    val visit = Consumer<SelectionKey> { key ->
        val v = key.attachment() as Visitor?
        if (v == null) xio!!.onAccept() else if (key.isValid) xio!!.ready(v, key.isWritable, key.isReadable)
    }

    actual fun select(xio: Xio, timeoutMs: Long) {
        this.xio = xio
        if (timeoutMs > 0) selector.select(visit, timeoutMs) else selector.select(visit)
    }

    actual fun accept(): Handle? {
        while (true) {
            val ch = try { server.accept() } catch (_: IOException) { return null } ?: return null
            try {
                ch.configureBlocking(false)
                ch.setOption(StandardSocketOptions.TCP_NODELAY, true)
                return Handle(ch)
            } catch (_: IOException) {
                try { ch.close() } catch (_: IOException) {}
            }
        }
    }

    actual fun register(v: Visitor) {
        try {
            v.handle.key = v.handle.channel.register(selector, SelectionKey.OP_READ, v)
        } catch (_: IOException) {
            v.open = false
            try { v.handle.channel.close() } catch (_: IOException) {}
        }
    }

    actual fun wantWrite(v: Visitor, on: Boolean) {
        v.handle.key?.interestOps(if (on) SelectionKey.OP_READ or SelectionKey.OP_WRITE else SelectionKey.OP_READ)
    }

    actual fun read(h: Handle, dst: ByteArray, off: Int, len: Int): Int {
        rx.clear().limit(minOf(len, rx.capacity()))
        val n = try { h.channel.read(rx) } catch (_: IOException) { return -1 }
        if (n > 0) {
            rx.flip()
            rx.get(dst, off, n)
        }
        return n
    }

    actual fun write(h: Handle, src: ByteArray, off: Int, len: Int): Int {
        val n = minOf(len, tx.capacity())
        tx.clear()
        tx.put(src, off, n)
        tx.flip()
        return try { h.channel.write(tx) } catch (_: IOException) { -1 }
    }

    actual fun close(v: Visitor) {
        v.handle.key?.cancel()
        try { v.handle.channel.close() } catch (_: IOException) {}
    }
}

actual fun epochSeconds(): Long = System.currentTimeMillis() / 1000

actual fun monoNanos(): Long = System.nanoTime()
