package challenge;

import io.micronaut.context.annotation.Context;
import jakarta.annotation.PreDestroy;
import org.sqlite.SQLiteConfig;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.*;

@Context
public final class Database {
    private static final String FEED_JSON = """
        SELECT CAST('{"posts":[' || coalesce(group_concat(post ORDER BY created_at DESC,id DESC), '') || ']}' AS BLOB) FROM (
            SELECT json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,
                'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id)) AS post,p.created_at,p.id
            FROM posts p JOIN users u ON u.id=p.user_id ORDER BY p.created_at DESC,p.id DESC LIMIT 20
        )
        """;
    private static final String POST_JSON = """
        SELECT CAST(json_object('post',json_object('id',p.id,'body',p.body,'created_at',p.created_at,
            'author',u.username,'like_count',(SELECT count(*) FROM likes l WHERE l.post_id=p.id))) AS BLOB)
        FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?
        """;
    private final DbExecutor executor;
    private final Connection connection;
    private final PreparedStatement health, feed, find, create, exists, like;

    public Database(DbExecutor executor) throws SQLException {
        this.executor = executor;
        Path path = Path.of(System.getenv().getOrDefault("SQLITE_PATH", "feed.db")).toAbsolutePath();
        if (!Files.isRegularFile(path)) throw new IllegalArgumentException("SQLITE_PATH must point to an existing seed copy: " + path);
        SQLiteConfig sqlite = new SQLiteConfig();
        sqlite.setSynchronous(SQLiteConfig.SynchronousMode.NORMAL);
        sqlite.setBusyTimeout(1000);
        sqlite.enforceForeignKeys(true);
        sqlite.setCacheSize(-8192);
        sqlite.setPragma(SQLiteConfig.Pragma.MMAP_SIZE, "268435456");
        connection = DriverManager.getConnection("jdbc:sqlite:" + path, sqlite.toProperties());
        try {
            try (var statement = connection.createStatement(); var rs = statement.executeQuery("PRAGMA journal_mode=WAL")) {
                if (!rs.next() || !"wal".equalsIgnoreCase(rs.getString(1))) throw new SQLException("WAL required");
            }
            health = connection.prepareStatement("SELECT 1");
            feed = connection.prepareStatement(FEED_JSON);
            find = connection.prepareStatement(POST_JSON);
            create = connection.prepareStatement("INSERT INTO posts(user_id,body) VALUES (?,?) RETURNING id,created_at");
            exists = connection.prepareStatement("SELECT 1 FROM posts WHERE id=?");
            like = connection.prepareStatement("""
                INSERT INTO likes(user_id,post_id) SELECT ?,id FROM posts WHERE id=?
                ON CONFLICT(user_id,post_id) DO NOTHING
                """);
            executor.start(connection);
        } catch (Exception e) { connection.close(); throw e; }
    }

    // Only the FIFO owner calls these methods. Its futures publish results after the batch commits.
    public boolean health() {
        try (var rs = health.executeQuery()) { return rs.next() && rs.getInt(1) == 1; }
        catch (SQLException e) { throw new IllegalStateException(e); }
    }
    public byte[] feed() {
        try (var rs = feed.executeQuery()) {
            if (!rs.next()) throw new SQLException("Feed returned no row");
            return rs.getBytes(1);
        } catch (SQLException e) { throw new IllegalStateException(e); }
    }
    public byte[] find(long id) {
        try {
            find.setLong(1, id);
            try (var rs = find.executeQuery()) {
                if (!rs.next()) throw new ApiException(404, "post not found");
                return rs.getBytes(1);
            }
        } catch (SQLException e) { throw new IllegalStateException(e); }
    }
    public Responses.Post create(JwtVerifier.User user, String body) {
        try {
            create.setLong(1, user.id()); create.setString(2, body);
            try (var rs = create.executeQuery()) {
                if (!rs.next()) throw new SQLException("INSERT returned no row");
                var post = new Responses.Post(rs.getLong(1), body, rs.getString(2), user.username(), 0);
                if (rs.next()) throw new SQLException("INSERT returned multiple rows");
                return post;
            }
        } catch (SQLException e) { throw new IllegalStateException(e); }
    }
    public boolean like(long user, long post) {
        try {
            like.setLong(1, user); like.setLong(2, post);
            if (like.executeUpdate() != 0) return false;
            exists.setLong(1, post);
            try (var rs = exists.executeQuery()) {
                if (!rs.next()) throw new ApiException(404, "post not found");
            }
            return true;
        } catch (SQLException e) { throw new IllegalStateException(e); }
    }
    @PreDestroy public void close() throws SQLException {
        executor.close();
        connection.close();
    }
}
