package io.github.wolches.challenge12.support;

import io.github.wolches.challenge12.seed.SeedDatabase;
import io.quarkus.test.common.QuarkusTestResourceLifecycleManager;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.sql.SQLException;
import java.util.Comparator;
import java.util.Map;
import java.util.stream.Stream;

/**
 * Starts the application on a fresh copy of {@code seed/feed.db}, the way test/run.sh does.
 * <p>
 * If the seed database is missing it is built first with {@link SeedDatabase} (the Java port of
 * seed/make-seed.sh) and its content checksum is verified, so the golden checks are meaningful.
 * That takes a minute or two once per checkout; the file is git-ignored.
 * <p>
 * The challenge root defaults to two levels above the working directory; override it with
 * {@code -Dchallenge.root=/path/to/twelve-dollar-server-challenge}.
 */
public class TestDatabaseResource implements QuarkusTestResourceLifecycleManager {

    private Path dir;

    public static Path root() {
        String configured = System.getProperty("challenge.root");
        Path root = configured != null ? Paths.get(configured) : Paths.get("..", "..");
        return root.toAbsolutePath().normalize();
    }

    @Override
    public Map<String, String> start() {
        try {
            Path seed = seedDatabase();
            dir = Files.createTempDirectory("challenge12-test-");
            Path db = dir.resolve("feed.db");
            Files.copy(seed, db);
            // application.properties builds the JDBC URLs from ${SQLITE_PATH}; the expansion also sees
            // config overrides, so this replaces the environment variable test/run.sh exports.
            return Map.of("SQLITE_PATH", db.toString().replace('\\', '/'));
        } catch (IOException | SQLException e) {
            throw new IllegalStateException("cannot prepare the test database", e);
        }
    }

    @Override
    public void stop() {
        if (dir == null) {
            return;
        }
        try (Stream<Path> files = Files.walk(dir)) {
            files.sorted(Comparator.reverseOrder()).forEach(p -> {
                try {
                    Files.deleteIfExists(p);
                } catch (IOException e) {
                    throw new UncheckedIOException(e);
                }
            });
        } catch (IOException | UncheckedIOException ignored) {
            // a leftover temp dir is not worth failing the build
        }
    }

    private static Path seedDatabase() throws IOException, SQLException {
        Path root = root();
        Path seed = root.resolve("seed").resolve("feed.db");
        if (Files.exists(seed)) {
            return seed;
        }
        Path schema = root.resolve("schema.sql");
        if (!Files.exists(schema)) {
            throw new IllegalStateException("schema.sql not found at " + schema + "; set -Dchallenge.root");
        }
        System.out.println("seed/feed.db not found, building it (" + seed + ")");
        SeedDatabase.build(schema, seed, seed.resolveSibling("tokens.json"));
        String sum = SeedDatabase.verify(seed);
        if (!SeedDatabase.EXPECTED_SHA256.equals(sum)) {
            Files.deleteIfExists(seed);
            throw new IllegalStateException("seed content sha256 " + sum + ", expected " + SeedDatabase.EXPECTED_SHA256);
        }
        return seed;
    }
}
