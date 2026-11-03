package io.github.wolches.challenge12.seed;

import io.github.wolches.challenge12.support.Jwt;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.List;
import java.util.Set;
import java.util.stream.Stream;

/**
 * Java port of ${proj_root}/seed/make-seed.sh
 */
public final class SeedDatabase {

    public static final String EXPECTED_SHA256 = "738d11ab3164c13177e347f75038d1e80b43c64e754544a42d1c32bd8d7bcf1d";

    private static final int SEED = 42;
    private static final int USERS = 50_000;
    private static final int POSTS = 500_000;
    private static final int LIKES = 2_000_000;
    private static final int TOKEN_COUNT = 20_000;

    private static final double END = 1_767_225_600_000d; // Date.parse('2026-01-01T00:00:00Z')
    private static final double DAY = 86_400_000d;

    private static final String[] ADJECTIVES = {"quiet", "brave", "clever", "sunny", "rusty", "swift", "lazy", "mellow",
            "sharp", "wild", "cosmic", "tiny", "golden", "silver", "happy", "grumpy", "curious", "lucky", "salty", "zesty"};
    private static final String[] NOUNS = {"otter", "falcon", "badger", "comet", "river", "maple", "pixel", "walrus",
            "ember", "glacier", "lantern", "meadow", "nebula", "orchid", "pebble", "quartz", "raven", "sparrow", "thistle", "willow"};
    private static final String[] WORDS = (
            "the a an and but or so because while after before just really very kind of sort of maybe probably definitely "
                    + "server database request latency cache index query benchmark droplet cpu memory disk network throughput users "
                    + "coffee morning weekend project deploy release bug feature test rewrite refactor commit branch merge review "
                    + "thinking building shipping learning reading writing debugging measuring watching waiting running walking "
                    + "fast slow small big cheap simple boring clever weird surprising honest careful lucky tired ready done broken "
                    + "today tonight tomorrow yesterday again finally never always sometimes usually rarely once twice "
                    + "this that these those here there everywhere nowhere something nothing everything anything someone "
                    + "nginx node postgres typescript express linux ubuntu systemd load steal swap kernel socket keepalive pool"
    ).trim().split("\\s+");
    private static final String[] OPENERS = {"Hot take:", "TIL", "Unpopular opinion:", "Note to self:", "PSA:", "Okay so",
            "Update:", "Honestly,", "Reminder:", "Question:"};
    private static final String[] PUNCTUATION = {".", ".", ".", "!", "?", "..."};

    private static final DateTimeFormatter ISO_MILLIS =
            DateTimeFormatter.ofPattern("uuuu-MM-dd'T'HH:mm:ss.SSS'Z'").withZone(ZoneOffset.UTC);

    private static final int BATCH = 10_000;

    private int state = SEED;
    private final List<String> usernames = new ArrayList<>(USERS);

    private SeedDatabase() {
    }

    public static void main(String[] args) throws Exception {
        Path root = Paths.get(args.length > 0 ? args[0] : "../..").toAbsolutePath().normalize();
        Path db = root.resolve("seed").resolve("feed.db");
        Path tokens = root.resolve("seed").resolve("tokens.json");
        build(root.resolve("schema.sql"), db, tokens);
        String sum = verify(db);
        if (EXPECTED_SHA256.equals(sum)) {
            System.out.println("OK: " + db + " (" + Files.size(db) / (1024 * 1024) + "M), content sha256 matches");
        } else {
            System.out.println("MISMATCH: content sha256 " + sum + ", expected " + EXPECTED_SHA256);
            System.exit(1);
        }
    }

    public static void build(Path schemaSql, Path db, Path tokensJson) throws IOException, SQLException {
        Files.deleteIfExists(db);
        Files.deleteIfExists(db.resolveSibling(db.getFileName() + "-wal"));
        Files.deleteIfExists(db.resolveSibling(db.getFileName() + "-shm"));
        Files.createDirectories(db.toAbsolutePath().getParent());

        SeedDatabase gen = new SeedDatabase();
        try (Connection c = DriverManager.getConnection("jdbc:sqlite:" + db)) {
            try (Statement st = c.createStatement()) {
                st.execute("PRAGMA journal_mode = OFF");
                st.execute("PRAGMA synchronous = OFF");
                for (String ddl : statements(Files.readString(schemaSql))) {
                    st.execute(ddl);
                }
            }
            c.setAutoCommit(false);
            gen.writeUsers(c);
            double[] postCreatedAt = gen.writePosts(c);
            gen.writeLikes(c, postCreatedAt);
            c.commit();
            c.setAutoCommit(true);
            try (Statement st = c.createStatement()) {
                st.execute("ANALYZE");
                st.execute("VACUUM");
                st.execute("PRAGMA journal_mode = WAL");
                st.execute("PRAGMA wal_checkpoint(TRUNCATE)");
            }
        }
        if (tokensJson != null) {
            Files.writeString(tokensJson, gen.tokensJson(), StandardCharsets.UTF_8);
        }
    }

    public static String verify(Path db) throws SQLException {
        MessageDigest sha;
        try {
            sha = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
        String[] queries = {
                "SELECT id, username, created_at FROM users ORDER BY id",
                "SELECT id, user_id, body, created_at FROM posts ORDER BY id",
                "SELECT user_id, post_id, created_at FROM likes ORDER BY post_id, user_id"};
        try (Connection c = DriverManager.getConnection("jdbc:sqlite:" + db); Statement st = c.createStatement()) {
            for (String query : queries) {
                try (ResultSet rs = st.executeQuery(query)) {
                    int columns = rs.getMetaData().getColumnCount();
                    StringBuilder line = new StringBuilder(640);
                    while (rs.next()) {
                        line.setLength(0);
                        for (int i = 1; i <= columns; i++) {
                            if (i > 1) {
                                line.append('|');
                            }
                            line.append(rs.getString(i));
                        }
                        line.append('\n');
                        sha.update(line.toString().getBytes(StandardCharsets.UTF_8));
                    }
                }
            }
        }
        return HexFormat.of().formatHex(sha.digest());
    }

    // --- generate.mjs, in call order ---------------------------------------------------------------

    private double rand() {
        state += 0x6d2b79f5;
        int t = state;
        t = (t ^ (t >>> 15)) * (t | 1);
        t ^= t + ((t ^ (t >>> 7)) * (t | 61));
        return Integer.toUnsignedLong(t ^ (t >>> 14)) / 4294967296d;
    }

    private int randInt(int lo, int hi) {
        return lo + (int) Math.floor(rand() * (hi - lo + 1));
    }

    private String pick(String[] arr) {
        return arr[(int) Math.floor(rand() * arr.length)];
    }

    private double expo(double mean) {
        return -mean * StrictMath.log(1 - rand());
    }

    private static String ts(double ms) {
        return ISO_MILLIS.format(Instant.ofEpochMilli((long) ms));
    }

    private String sentence() {
        int n = randInt(4, 14);
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < n; i++) {
            if (i > 0) {
                s.append(' ');
            }
            s.append(pick(WORDS));
        }
        s.setCharAt(0, Character.toUpperCase(s.charAt(0)));
        return s.append(pick(PUNCTUATION)).toString();
    }

    private String postBody() {
        List<String> parts = new ArrayList<>(4);
        if (rand() < 0.25) {
            parts.add(pick(OPENERS));
        }
        int sentences = randInt(1, 3);
        for (int i = 0; i < sentences; i++) {
            parts.add(sentence());
        }
        String body = String.join(" ", parts);
        if (body.length() > 500) {
            body = body.substring(0, 497) + "...";
        }
        return body;
    }

    private void writeUsers(Connection c) throws SQLException {
        try (PreparedStatement ps = c.prepareStatement("INSERT INTO users (id, username, created_at) VALUES (?, ?, ?)")) {
            for (int i = 0; i < USERS; i++) {
                int id = i + 1;
                String username = pick(ADJECTIVES) + "_" + pick(NOUNS) + "_" + id;
                usernames.add(username);
                ps.setInt(1, id);
                ps.setString(2, username);
                ps.setString(3, ts(END - 730 * DAY + rand() * 365 * DAY));
                ps.addBatch();
                if (id % BATCH == 0) {
                    ps.executeBatch();
                }
            }
            ps.executeBatch();
        }
    }

    private double[] writePosts(Connection c) throws SQLException {
        double postStart = END - 365 * DAY;
        double step = (365 * DAY) / POSTS;
        double[] postCreatedAt = new double[POSTS];
        try (PreparedStatement ps = c.prepareStatement(
                "INSERT INTO posts (id, user_id, body, created_at) VALUES (?, ?, ?, ?)")) {
            for (int i = 0; i < POSTS; i++) {
                int userId = 1 + (int) Math.floor(USERS * rand() * rand());
                double createdAt = postStart + i * step + rand() * step * 0.9;
                postCreatedAt[i] = createdAt;
                ps.setInt(1, i + 1);
                ps.setInt(2, userId);
                ps.setString(3, postBody());
                ps.setString(4, ts(createdAt));
                ps.addBatch();
                if ((i + 1) % BATCH == 0) {
                    ps.executeBatch();
                }
            }
            ps.executeBatch();
        }
        return postCreatedAt;
    }

    private void writeLikes(Connection c, double[] postCreatedAt) throws SQLException {
        double meanPerPost = (double) LIKES / POSTS;
        double popularMean = meanPerPost * 20;
        double normalMean = Math.max(0.05, (meanPerPost - 0.01 * popularMean + 0.5) / 0.99);
        int cap = (int) Math.min(USERS, Math.max(50, Math.round(popularMean * 10)));
        long likeTotal = 0;
        int[] perPost = new int[POSTS];
        for (int i = 0; i < POSTS; i++) {
            perPost[i] = (int) Math.min(cap, Math.floor(expo(rand() < 0.01 ? popularMean : normalMean)));
            likeTotal += perPost[i];
        }
        int postIdx = 0;
        int remaining = 0;
        Set<Integer> seen = new HashSet<>();
        try (PreparedStatement ps = c.prepareStatement(
                "INSERT INTO likes (user_id, post_id, created_at) VALUES (?, ?, ?)")) {
            for (long row = 0; row < likeTotal; row++) {
                while (remaining == 0) {
                    postIdx++;
                    remaining = perPost[postIdx - 1];
                    seen.clear();
                }
                remaining--;
                int userId;
                do {
                    userId = randInt(1, USERS);
                } while (seen.contains(userId));
                seen.add(userId);
                ps.setInt(1, userId);
                ps.setInt(2, postIdx);
                ps.setString(3, ts(postCreatedAt[postIdx - 1] + rand() * 7 * DAY));
                ps.addBatch();
                if ((row + 1) % BATCH == 0) {
                    ps.executeBatch();
                }
            }
            ps.executeBatch();
        }
    }

    private String tokensJson() {
        long iat = Jwt.now();
        StringBuilder json = new StringBuilder(TOKEN_COUNT * 200).append('[');
        for (int i = 0; i < TOKEN_COUNT; i++) {
            String username = usernames.get(i);
            String token = Jwt.sign("{\"sub\":\"" + (i + 1) + "\",\"username\":\"" + username + "\",\"iat\":" + iat
                    + ",\"exp\":" + (iat + 3650L * 86400) + "}");
            if (i > 0) {
                json.append(',');
            }
            json.append("{\"id\":").append(i + 1).append(",\"username\":\"").append(username)
                    .append("\",\"token\":\"").append(token).append("\"}");
        }
        return json.append(']').toString();
    }

    private static String[] statements(String sql) {
        String noComments = sql.lines()
                .filter(line -> !line.stripLeading().startsWith("--"))
                .reduce("", (a, b) -> a + b + "\n");
        return Stream.of(noComments.split(";"))
                .map(String::strip)
                .filter(s -> !s.isEmpty())
                .toArray(String[]::new);
    }
}
