package challenge;

import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.stream.Stream;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Repository;
import org.sqlite.SQLiteConfig;
import org.sqlite.SQLiteDataSource;

/**
 * Owns the SQLite connections, each with its statements prepared once. A request borrows one
 * connection and its statements exclusively, so statements are never shared between threads.
 * Statements cache query plans, never results.
 *
 * <p>Three read-only connections serve reads. One writer connection serves every write, so writes
 * queue in Java instead of colliding in SQLite, where a waiting writer sleeps and retries.
 */
@Repository
class PostRepository implements AutoCloseable {
    private static final int READERS = 3;
    private static final long BORROW_TIMEOUT_MILLIS = 10_000;
    private static final String POST_JSON =
            """
            json_object('id', p.id, 'body', p.body, 'created_at', p.created_at,
                        'author', u.username,
                        'like_count', (SELECT count(*) FROM likes l WHERE l.post_id = p.id))
            """;
    // The response is built in SQLite and sent as is. The aggregate's ORDER BY fixes the order
    // of the posts in the array.
    private static final String FEED =
            """
            SELECT json_object('posts', json_group_array(json(post) ORDER BY created_at DESC, id DESC))
              FROM (SELECT p.id, p.created_at, %s AS post
                      FROM posts p JOIN users u ON u.id = p.user_id
                     ORDER BY p.created_at DESC, p.id DESC LIMIT 20)
            """
                    .formatted(POST_JSON);
    private static final String POST =
            """
            SELECT json_object('post', %s)
              FROM posts p JOIN users u ON u.id = p.user_id
             WHERE p.id = ?
            """
                    .formatted(POST_JSON);

    private final Pool<ReadSession> readers;
    private final Pool<WriteSession> writer;

    PostRepository(@Value("${SQLITE_PATH}") String path) throws SQLException {
        if (!Files.isRegularFile(Path.of(path))) {
            throw new IllegalArgumentException("SQLITE_PATH must name an existing database file");
        }
        SQLiteConfig sqlite = new SQLiteConfig();
        sqlite.setJournalMode(SQLiteConfig.JournalMode.WAL);
        sqlite.setSynchronous(SQLiteConfig.SynchronousMode.NORMAL);
        sqlite.enforceForeignKeys(true);
        sqlite.setBusyTimeout(5000);
        // Otherwise every insert runs and prepares an extra SELECT last_insert_rowid().
        sqlite.setGetGeneratedKeys(false);
        SQLiteDataSource source = new SQLiteDataSource(sqlite);
        source.setUrl("jdbc:sqlite:" + Path.of(path).toAbsolutePath());
        List<ReadSession> readSessions = new ArrayList<>();
        List<WriteSession> writeSessions = new ArrayList<>();
        try {
            for (int i = 0; i < READERS; i++) {
                readSessions.add(new ReadSession(source.getConnection()));
            }
            writeSessions.add(new WriteSession(source.getConnection()));
        } catch (SQLException | RuntimeException exception) {
            Stream.concat(readSessions.stream(), writeSessions.stream()).forEach(Session::close);
            throw exception;
        }
        readers = new Pool<>(readSessions);
        writer = new Pool<>(writeSessions);
    }

    void checkHealth() {
        readers.use(
                session -> {
                    try (ResultSet rows = session.health.executeQuery()) {
                        if (!rows.next()) {
                            throw new SQLException("SELECT 1 returned no row");
                        }
                    }
                    return null;
                });
    }

    /** The feed response body, {"posts":[...]}, as UTF-8 JSON. */
    byte[] feed() {
        return readers.use(session -> json(session.feed));
    }

    /** The post response body, {"post":{...}}, as UTF-8 JSON. */
    byte[] get(long id) {
        byte[] post =
                readers.use(
                        session -> {
                            session.post.setLong(1, id);
                            return json(session.post);
                        });
        if (post == null) {
            throw new ApiException(404, "post not found");
        }
        return post;
    }

    Responses.Post create(Authentication.User user, String body) {
        return writer.use(
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

    /** Returns whether the user had already liked the post. */
    boolean like(long userId, long postId) {
        return writer.use(
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

    /** All connections, readers first, for tests that inspect connection settings. */
    List<Connection> connections() {
        return Stream.concat(readers.sessions.stream(), writer.sessions.stream())
                .map(session -> session.connection)
                .toList();
    }

    @Override
    public void close() {
        readers.close();
        writer.close();
    }

    /** The first column of the only row as raw UTF-8 bytes, or null without a row. */
    private static byte[] json(PreparedStatement statement) throws SQLException {
        try (ResultSet rows = statement.executeQuery()) {
            return rows.next() ? rows.getBytes(1) : null;
        }
    }

    private interface Work<S, T> {
        T run(S session) throws SQLException;
    }

    /** Sessions that requests borrow in arrival order. */
    private static final class Pool<S extends Session> implements AutoCloseable {
        final List<S> sessions;
        private final BlockingQueue<S> idle;

        Pool(List<S> sessions) {
            this.sessions = List.copyOf(sessions);
            idle = new ArrayBlockingQueue<>(sessions.size(), true, sessions);
        }

        <T> T use(Work<S, T> work) {
            S session;
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

        @Override
        public void close() {
            sessions.forEach(Session::close);
        }
    }

    private abstract static class Session {
        final Connection connection;

        Session(Connection connection) {
            this.connection = connection;
        }

        PreparedStatement prepare(String sql) throws SQLException {
            try {
                return connection.prepareStatement(sql);
            } catch (SQLException exception) {
                close();
                throw exception;
            }
        }

        void close() {
            try {
                connection.close();
            } catch (SQLException ignored) {
                // Closing is best effort during shutdown.
            }
        }
    }

    private static final class ReadSession extends Session {
        final PreparedStatement health;
        final PreparedStatement feed;
        final PreparedStatement post;

        ReadSession(Connection connection) throws SQLException {
            super(connection);
            try (Statement statement = connection.createStatement()) {
                statement.execute("PRAGMA query_only = 1");
            } catch (SQLException exception) {
                close();
                throw exception;
            }
            health = prepare("SELECT 1");
            feed = prepare(FEED);
            post = prepare(POST);
        }
    }

    private static final class WriteSession extends Session {
        final PreparedStatement create;
        final PreparedStatement postExists;
        final PreparedStatement like;

        WriteSession(Connection connection) throws SQLException {
            super(connection);
            create =
                    prepare(
                            "INSERT INTO posts (user_id, body) VALUES (?, ?)"
                                    + " RETURNING id, created_at");
            postExists = prepare("SELECT 1 FROM posts WHERE id = ?");
            like =
                    prepare(
                            "INSERT INTO likes (user_id, post_id) VALUES (?, ?)"
                                    + " ON CONFLICT (user_id, post_id) DO NOTHING");
        }
    }
}
