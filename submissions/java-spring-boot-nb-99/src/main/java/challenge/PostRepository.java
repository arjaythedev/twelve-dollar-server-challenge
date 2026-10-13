package challenge;

import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Repository;
import org.sqlite.SQLiteConfig;
import org.sqlite.SQLiteDataSource;

/**
 * Owns a fixed set of SQLite connections, each with its statements prepared once. A request borrows
 * one connection and its statements exclusively, so statements are never shared between threads.
 * Statements cache query plans, never results.
 */
@Repository
class PostRepository implements AutoCloseable {
    private static final int CONNECTIONS = 4;
    private static final long BORROW_TIMEOUT_MILLIS = 10_000;
    private static final String SELECT_POST =
            """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;

    private final List<Session> sessions = new ArrayList<>();
    private final BlockingQueue<Session> idle = new ArrayBlockingQueue<>(CONNECTIONS);

    PostRepository(@Value("${SQLITE_PATH}") String path) throws SQLException {
        if (!Files.isRegularFile(Path.of(path))) {
            throw new IllegalArgumentException("SQLITE_PATH must name an existing database file");
        }
        SQLiteConfig sqlite = new SQLiteConfig();
        sqlite.setJournalMode(SQLiteConfig.JournalMode.WAL);
        sqlite.setSynchronous(SQLiteConfig.SynchronousMode.NORMAL);
        sqlite.enforceForeignKeys(true);
        sqlite.setBusyTimeout(5000);
        SQLiteDataSource source = new SQLiteDataSource(sqlite);
        source.setUrl("jdbc:sqlite:" + Path.of(path).toAbsolutePath());
        try {
            for (int i = 0; i < CONNECTIONS; i++) {
                Session session = new Session(source.getConnection());
                sessions.add(session);
                idle.add(session);
            }
        } catch (SQLException | RuntimeException exception) {
            close();
            throw exception;
        }
    }

    void checkHealth() {
        use(
                session -> {
                    try (ResultSet rows = session.health.executeQuery()) {
                        if (!rows.next()) {
                            throw new SQLException("SELECT 1 returned no row");
                        }
                    }
                    return null;
                });
    }

    List<Responses.Post> feed() {
        return use(session -> posts(session.feed));
    }

    Responses.Post get(long id) {
        List<Responses.Post> posts =
                use(
                        session -> {
                            session.post.setLong(1, id);
                            return posts(session.post);
                        });
        if (posts.isEmpty()) {
            throw new ApiException(404, "post not found");
        }
        return posts.getFirst();
    }

    Responses.Post create(Authentication.User user, String body) {
        return use(
                session -> {
                    session.create.setLong(1, user.id());
                    session.create.setString(2, body);
                    // Closing the RETURNING result resets the statement, which completes
                    // auto-commit before the response is written.
                    try (ResultSet row = session.create.executeQuery()) {
                        if (!row.next()) {
                            throw new SQLException("INSERT RETURNING returned no row");
                        }
                        return new Responses.Post(
                                row.getLong(1), body, row.getString(2), user.username(), 0);
                    }
                });
    }

    boolean like(long userId, long postId) {
        return use(
                session -> {
                    session.postExists.setLong(1, postId);
                    try (ResultSet row = session.postExists.executeQuery()) {
                        if (!row.next()) {
                            throw new ApiException(404, "post not found");
                        }
                    }
                    session.like.setLong(1, userId);
                    session.like.setLong(2, postId);
                    return session.like.executeUpdate() == 0;
                });
    }

    /** The pooled connections, for tests that inspect connection settings. */
    List<Connection> connections() {
        return sessions.stream().map(session -> session.connection).toList();
    }

    @Override
    public void close() {
        for (Session session : sessions) {
            try {
                session.connection.close();
            } catch (SQLException ignored) {
                // Closing is best effort during shutdown.
            }
        }
    }

    private <T> T use(Work<T> work) {
        Session session;
        try {
            session = idle.poll(BORROW_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("Interrupted while waiting for a connection");
        }
        if (session == null) {
            throw new IllegalStateException("No database connection became available");
        }
        try {
            return work.run(session);
        } catch (SQLException exception) {
            throw new IllegalStateException("Database operation failed", exception);
        } finally {
            idle.add(session);
        }
    }

    private static List<Responses.Post> posts(PreparedStatement statement) throws SQLException {
        try (ResultSet rows = statement.executeQuery()) {
            List<Responses.Post> posts = new ArrayList<>(20);
            while (rows.next()) {
                posts.add(
                        new Responses.Post(
                                rows.getLong(1),
                                rows.getString(2),
                                rows.getString(3),
                                rows.getString(4),
                                rows.getLong(5)));
            }
            return posts;
        }
    }

    private interface Work<T> {
        T run(Session session) throws SQLException;
    }

    private static final class Session {
        final Connection connection;
        final PreparedStatement health;
        final PreparedStatement feed;
        final PreparedStatement post;
        final PreparedStatement create;
        final PreparedStatement postExists;
        final PreparedStatement like;

        Session(Connection connection) throws SQLException {
            this.connection = connection;
            try {
                health = connection.prepareStatement("SELECT 1");
                feed =
                        connection.prepareStatement(
                                SELECT_POST + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
                post = connection.prepareStatement(SELECT_POST + " WHERE p.id = ?");
                create =
                        connection.prepareStatement(
                                "INSERT INTO posts (user_id, body) VALUES (?, ?)"
                                        + " RETURNING id, created_at");
                postExists = connection.prepareStatement("SELECT 1 FROM posts WHERE id = ?");
                like =
                        connection.prepareStatement(
                                "INSERT INTO likes (user_id, post_id) VALUES (?, ?)"
                                        + " ON CONFLICT (user_id, post_id) DO NOTHING");
            } catch (SQLException exception) {
                connection.close();
                throw exception;
            }
        }
    }
}
