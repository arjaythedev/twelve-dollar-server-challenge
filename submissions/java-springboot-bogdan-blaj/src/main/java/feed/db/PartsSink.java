package feed.db;

import java.lang.foreign.MemorySegment;

/**
 * Receives a response body that SQLite produces in pieces (one per result row). On success {@link #begin} is
 * called once, then {@link #append} any number of times, then {@link #end} once. Each {@code src} is only valid
 * during its call: copy it out. If the producer throws after {@link #begin}, the partial body must be discarded.
 */
public interface PartsSink {

    void begin();

    void append(MemorySegment src, int len);

    void end();
}
