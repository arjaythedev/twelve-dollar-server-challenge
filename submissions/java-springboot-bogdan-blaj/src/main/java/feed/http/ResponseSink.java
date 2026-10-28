package feed.http;

import feed.db.BytesSink;
import feed.db.PartsSink;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.ByteBufAllocator;

import java.lang.foreign.MemorySegment;

/**
 * Builds a complete response around a body handed over by SQLite. One reused instance per event loop: call
 * {@link #prepare}, pass it to a {@code FeedDb} method, then {@link #take} the buffer (or {@link #discard} it on error).
 *
 * <p>As a {@link PartsSink} the body length is only known at the end, so the pieces are written after
 * {@link Responses#MAX_HEAD} bytes of headroom and the header is filled in right before them at {@link #end}.
 */
final class ResponseSink implements BytesSink, PartsSink {

    /** Lets a raw address serve as a copy destination without allocating a segment per copy. */
    private static final MemorySegment ALL_MEMORY = MemorySegment.NULL.reinterpret(Long.MAX_VALUE);
    private static final int PARTS_SLACK = 512;

    private ByteBufAllocator alloc;
    private int status;
    private boolean close;
    private ByteBuf out;
    /** Parts mode: {@link #begin} ran and {@link #end} has not yet. */
    private boolean partial;
    private int lastPartsBodyLen = 8192 - PARTS_SLACK;

    ResponseSink prepare(ByteBufAllocator alloc, int status, boolean close) {
        discard();
        this.alloc = alloc;
        this.status = status;
        this.close = close;
        return this;
    }

    @Override
    public void accept(MemorySegment src, int len) {
        discard();
        ByteBuf b = Responses.head(alloc, status, close, len);
        int wi = b.writerIndex();
        MemorySegment.copy(src, 0, MemorySegment.ofBuffer(b.internalNioBuffer(wi, len)), 0, len);
        b.writerIndex(wi + len);
        out = b;
    }

    @Override
    public void begin() {
        discard();
        ByteBuf b = alloc.directBuffer(Responses.MAX_HEAD + lastPartsBodyLen + PARTS_SLACK);
        b.writerIndex(Responses.MAX_HEAD);
        out = b;
        partial = true;
    }

    @Override
    public void append(MemorySegment src, int len) {
        ByteBuf b = out;
        b.ensureWritable(len);
        int wi = b.writerIndex();
        if (b.hasMemoryAddress()) {
            MemorySegment.copy(src, 0, ALL_MEMORY, b.memoryAddress() + wi, len);
        } else {
            MemorySegment.copy(src, 0, MemorySegment.ofBuffer(b.internalNioBuffer(wi, len)), 0, len);
        }
        b.writerIndex(wi + len);
    }

    @Override
    public void end() {
        ByteBuf b = out;
        int bodyLen = b.writerIndex() - Responses.MAX_HEAD;
        b.readerIndex(Responses.headBefore(b, Responses.MAX_HEAD, status, close, bodyLen));
        lastPartsBodyLen = bodyLen;
        partial = false;
    }

    /** The finished response, or null if the sink was not called (or a parts body was not ended). The caller owns it. */
    ByteBuf take() {
        if (partial) {
            discard();
        }
        ByteBuf b = out;
        out = null;
        alloc = null;
        return b;
    }

    void discard() {
        partial = false;
        ByteBuf b = out;
        if (b != null) {
            out = null;
            b.release();
        }
    }
}
