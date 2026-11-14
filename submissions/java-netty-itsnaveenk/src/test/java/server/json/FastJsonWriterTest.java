package server.json;

import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import org.junit.jupiter.api.Test;
import server.db.Database;

import java.nio.charset.StandardCharsets;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;

public class FastJsonWriterTest {

    @Test
    void testWriteHealth() {
        ByteBuf buf = Unpooled.buffer();
        FastJsonWriter.writeHealth(buf, 42);
        String json = buf.toString(StandardCharsets.UTF_8);
        buf.release();
        assertEquals("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":42}", json);
    }

    @Test
    void testWriteError() {
        ByteBuf buf = Unpooled.buffer();
        FastJsonWriter.writeError(buf, "not found");
        String json = buf.toString(StandardCharsets.UTF_8);
        buf.release();
        assertEquals("{\"error\":\"not found\"}", json);
    }

    @Test
    void testWritePostGolden1() {
        ByteBuf buf = Unpooled.buffer();
        FastJsonWriter.writePostResponse(buf, 1,
                "Fast tomorrow small bug big droplet. Weird cache really project rarely everything?",
                "2025-01-01T00:00:34.152Z",
                "happy_nebula_10241",
                11);
        String json = buf.toString(StandardCharsets.UTF_8);
        buf.release();

        String expected = "{\"post\":{\"id\":1,\"body\":\"Fast tomorrow small bug big droplet. Weird cache really project rarely everything?\",\"created_at\":\"2025-01-01T00:00:34.152Z\",\"author\":\"happy_nebula_10241\",\"like_count\":11}}";
        assertEquals(expected, json);
    }

    @Test
    void testWriteLikeResponse() {
        ByteBuf buf1 = Unpooled.buffer();
        FastJsonWriter.writeLikeResponse(buf1, false, 500);
        String json1 = buf1.toString(StandardCharsets.UTF_8);
        buf1.release();
        assertEquals("{\"liked\":true,\"already_liked\":false,\"post_id\":500}", json1);

        ByteBuf buf2 = Unpooled.buffer();
        FastJsonWriter.writeLikeResponse(buf2, true, 500);
        String json2 = buf2.toString(StandardCharsets.UTF_8);
        buf2.release();
        assertEquals("{\"liked\":true,\"already_liked\":true,\"post_id\":500}", json2);
    }

    @Test
    void testWriteFeedResponse() {
        ByteBuf buf = Unpooled.buffer();
        List<Database.Post> posts = List.of(
                new Database.Post(2, "Second", "2025-01-02T00:00:00.000Z", "bob", 1),
                new Database.Post(1, "First", "2025-01-01T00:00:00.000Z", "alice", 0)
        );
        FastJsonWriter.writeFeedResponse(buf, posts);
        String json = buf.toString(StandardCharsets.UTF_8);
        buf.release();

        String expected = "{\"posts\":[{\"id\":2,\"body\":\"Second\",\"created_at\":\"2025-01-02T00:00:00.000Z\",\"author\":\"bob\",\"like_count\":1},{\"id\":1,\"body\":\"First\",\"created_at\":\"2025-01-01T00:00:00.000Z\",\"author\":\"alice\",\"like_count\":0}]}";
        assertEquals(expected, json);
    }

    @Test
    void testEscapingAndSpecialCharacters() {
        ByteBuf buf = Unpooled.buffer();
        FastJsonWriter.writeJsonString(buf, "test: <b>&amp;</b> \"quoted\" / café ✓");
        String json = buf.toString(StandardCharsets.UTF_8);
        buf.release();

        assertEquals("\"test: <b>&amp;</b> \\\"quoted\\\" / café ✓\"", json);
    }
}
