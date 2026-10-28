package feed.db;

import java.lang.foreign.MemorySegment;

/** Receives a response body produced by SQLite. {@code src} is only valid during the call: copy it out. */
@FunctionalInterface
public interface BytesSink {
    void accept(MemorySegment src, int len);
}
