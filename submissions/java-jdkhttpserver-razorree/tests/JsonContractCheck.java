package challenge;

import com.fasterxml.jackson.core.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.nio.charset.StandardCharsets;

public final class JsonContractCheck {
    private static final JsonFactory ORACLE = new JsonFactory();
    private static final AtomicInteger checks = new AtomicInteger();

    private static void oracle(JsonGenerator out, Object value) throws IOException {
        switch (value) {
            case null -> out.writeNull();
            case String s -> out.writeString(s);
            case Boolean b -> out.writeBoolean(b);
            case Integer i -> out.writeNumber(i);
            case Long l -> out.writeNumber(l);
            case Map<?, ?> map -> {
                out.writeStartObject();
                for (var e : map.entrySet()) {
                    out.writeFieldName((String) e.getKey());
                    oracle(out, e.getValue());
                }
                out.writeEndObject();
            }
            case List<?> list -> {
                out.writeStartArray();
                for (Object item : list) oracle(out, item);
                out.writeEndArray();
            }
            default -> throw new IOException("Unsupported oracle value");
        }
    }

    private static void check(Object value) throws IOException { check(value, value); }

    private static void check(Object value, Object expectedValue) throws IOException {
        ByteArrayOutputStream expected = new ByteArrayOutputStream();
        try (JsonGenerator out = ORACLE.createGenerator(expected)) { oracle(out, expectedValue); }
        byte[] actual = Json.encode(value), reference = expected.toByteArray();
        if (!Arrays.equals(actual, reference)) {
            int index = Arrays.mismatch(actual, reference);
            throw new AssertionError("JSON byte mismatch, case " + checks + " at " + index
                + "; actual " + HexFormat.of().formatHex(actual, Math.max(0, index), Math.min(actual.length, index + 20))
                + "; expected " + HexFormat.of().formatHex(reference, Math.max(0, index), Math.min(reference.length, index + 20)));
        }
        checks.incrementAndGet();
    }

    private static final class RowsFixture implements Sqlite.Rows {
        final List<Database.Post> posts;
        int index = -1;
        RowsFixture(Database.Post... posts) { this.posts = List.of(posts); }
        public boolean next() { return ++index < posts.size(); }
        public long getLong(int column) {
            Database.Post post = posts.get(index);
            return column == 1 ? post.id() : post.likeCount();
        }
        public int getInt(int column) { return (int) getLong(column); }
        public String getString(int column) { throw new AssertionError("Read path created a String"); }
        public byte[] getBytes(int column) {
            Database.Post post = posts.get(index);
            return switch (column) {
                case 2 -> post.body().getBytes(StandardCharsets.UTF_8);
                case 3 -> post.createdAt().getBytes(StandardCharsets.UTF_8);
                case 4 -> post.author().getBytes(StandardCharsets.UTF_8);
                default -> throw new AssertionError("Unexpected column " + column);
            };
        }
        public void close() {}
    }

    private static void checkDirect(Database.Post post) throws Exception {
        RowsFixture row = new RowsFixture(post);
        if (!row.next()) throw new AssertionError("Missing fixture row");
        byte[] expected = Json.encode(Json.object("post", post));
        byte[] actual = Json.postBytes(row);
        if (!Arrays.equals(actual, expected)) {
            int at = Arrays.mismatch(actual, expected);
            throw new AssertionError("Direct post JSON differs from Jackson at " + at + ": actual="
                + HexFormat.of().formatHex(actual, Math.max(0, at - 5), Math.min(actual.length, at + 10))
                + " expected=" + HexFormat.of().formatHex(expected, Math.max(0, at - 5), Math.min(expected.length, at + 10)));
        }
        row = new RowsFixture(post, post);
        expected = Json.encode(Json.object("posts", List.of(post, post)));
        actual = Json.feedBytes(row);
        if (!Arrays.equals(actual, expected)) throw new AssertionError("Direct feed JSON differs from Jackson at " + Arrays.mismatch(actual, expected));
        checks.addAndGet(2);
    }

    public static void main(String[] args) throws Exception {
        for (byte[] malformed : List.of(
                "{\"x\":1,\"x\":2}".getBytes(StandardCharsets.UTF_8),
                "\"\\uD800\"".getBytes(StandardCharsets.UTF_8),
                "[1,]".getBytes(StandardCharsets.UTF_8),
                new byte[]{'"', (byte) 0xc3, '"'})) {
            try { Json.parse(malformed); throw new AssertionError("Malformed JSON accepted"); }
            catch (java.io.IOException expected) { }
        }
        Object parsed = Json.parse("{\"n\":1.12345678901234567890,\"s\":\"😀\"}".getBytes(StandardCharsets.UTF_8));
        if (!(parsed instanceof Map<?, ?> values) || !new java.math.BigDecimal("1.12345678901234567890").equals(values.get("n")))
            throw new AssertionError("JSON number precision changed");
        check(Json.object("id", Long.MAX_VALUE, "zero", 0, "min", Long.MIN_VALUE,
                          "negative", -123456789L, "null", null, "true", true, "false", false));
        check(List.of(Json.object("posts", List.of()), "quote\"\\slash/", "\b\f\n\r\t\u0000\u000b"));
        StringBuilder allBmp = new StringBuilder();
        for (int c = 0; c <= 0xffff; c++)
            if (!Character.isSurrogate((char) c)) allBmp.append((char) c);
        check(allBmp.toString());
        StringBuilder supplementary = new StringBuilder();
        for (int cp = 0x10000; cp <= 0x10ffff; cp += 257) supplementary.appendCodePoint(cp);
        check(supplementary.toString());
        for (String body : List.of(allBmp.toString(), supplementary.toString())) {
            var post = new Database.Post(Long.MAX_VALUE, body, "2026-10-23 12:00:00", body, 0);
            check(Json.object("post", post), Json.object("post", Json.object(
                "id", Long.MAX_VALUE, "body", body, "created_at", "2026-10-23 12:00:00",
                "author", body, "like_count", 0)));
            checkDirect(post);
        }
        Random random = new Random(8714);
        for (int test = 0; test < 128; test++) {
            StringBuilder text = new StringBuilder();
            for (int i = 0, count = random.nextInt(1000); i < count; i++) {
                int cp;
                do { cp = random.nextInt(0x110000); } while (cp >= 0xd800 && cp <= 0xdfff);
                text.appendCodePoint(cp);
            }
            check(Json.object("body", text.toString(), "n", random.nextLong()));
            checkDirect(new Database.Post(random.nextLong(), text.toString(), "2026-10-23 12:00:00", text.toString(), random.nextLong()));
        }
        // Concurrent encoding must preserve bytes and isolate request data.
        try (ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor()) {
            List<Future<?>> tasks = new ArrayList<>();
            CyclicBarrier barrier = new CyclicBarrier(16);
            for (int worker = 0; worker < 16; worker++) {
                final int id = worker;
                tasks.add(executor.submit(() -> {
                    barrier.await();
                    for (int i = 0; i < 128; i++)
                        check(Json.object("worker", id, "body", "café😀 " + id + " / " + i));
                    return null;
                }));
            }
            for (Future<?> task : tasks) task.get(30, TimeUnit.SECONDS);
        }
        System.out.println("PASS " + checks + " exact JSON byte comparisons against Jackson, including large responses");
    }
}
