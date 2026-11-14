package server.db;

import io.netty.buffer.ByteBuf;
import server.json.FastJsonWriter;

import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

public final class Database implements AutoCloseable {

    public record Post(long id, String body, String createdAt, String author, int likeCount) {}
    public record InsertPostResult(long id, String createdAt) {}
    public enum LikeResult {
        CREATED,
        ALREADY_LIKED,
        NOT_FOUND
    }

    private final Connection connRead;
    private final Connection connWrite;

    private final PreparedStatement healthStmt;
    private final PreparedStatement feedStmt;
    private final PreparedStatement postByIdStmt;

    private final PreparedStatement insertPostStmt;
    private final PreparedStatement insertLikeStmt;
    private final PreparedStatement postExistsStmt;

    private static final byte[] FEED_WRAPPER_PREFIX = "{\"posts\":[".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
    private static final byte[] POST_WRAPPER_PREFIX = "{\"post\":".getBytes(java.nio.charset.StandardCharsets.US_ASCII);

    private static final String POST_SELECT = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;

    private static final String FEED_SQL = POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    private static final String POST_BY_ID_SQL = POST_SELECT + " WHERE p.id = ?";
    private static final String INSERT_POST_SQL = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at";
    private static final String INSERT_LIKE_SQL = """
            INSERT INTO likes (user_id, post_id)
            SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
            ON CONFLICT (user_id, post_id) DO NOTHING
            """;
    private static final String POST_EXISTS_SQL = "SELECT 1 FROM posts WHERE id = ?";

    public Database(String sqlitePath) throws SQLException {
        // Read connection: optimized for concurrent non-blocking reads in WAL mode
        this.connRead = DriverManager.getConnection("jdbc:sqlite:" + sqlitePath);
        this.connRead.setAutoCommit(true);
        try (Statement s = connRead.createStatement()) {
            s.execute("PRAGMA journal_mode = WAL;");
            s.execute("PRAGMA synchronous = NORMAL;");
            s.execute("PRAGMA mmap_size = 1073741824;");
            s.execute("PRAGMA cache_size = -65536;");
            s.execute("PRAGMA temp_store = MEMORY;");
            s.execute("PRAGMA busy_timeout = 5000;");
            s.execute("PRAGMA query_only = 1;");
            s.execute("PRAGMA threads = 0;");
            s.execute("PRAGMA wal_autocheckpoint = 20000;");
        }

        // Write connection: dedicated connection for durable synchronous writes in WAL mode
        this.connWrite = DriverManager.getConnection("jdbc:sqlite:" + sqlitePath);
        this.connWrite.setAutoCommit(true);
        try (Statement s = connWrite.createStatement()) {
            s.execute("PRAGMA synchronous = NORMAL;");
            s.execute("PRAGMA mmap_size = 1073741824;");
            s.execute("PRAGMA cache_size = -65536;");
            s.execute("PRAGMA temp_store = MEMORY;");
            s.execute("PRAGMA busy_timeout = 5000;");
            s.execute("PRAGMA threads = 0;");
            s.execute("PRAGMA wal_autocheckpoint = 20000;");
        }

        this.healthStmt = connRead.prepareStatement("SELECT 1");
        this.feedStmt = connRead.prepareStatement(FEED_SQL);
        this.postByIdStmt = connRead.prepareStatement(POST_BY_ID_SQL);

        this.insertPostStmt = connWrite.prepareStatement(INSERT_POST_SQL);
        this.insertLikeStmt = connWrite.prepareStatement(INSERT_LIKE_SQL);
        this.postExistsStmt = connWrite.prepareStatement(POST_EXISTS_SQL);
    }

    public boolean checkHealth() {
        try (ResultSet rs = healthStmt.executeQuery()) {
            return rs.next();
        } catch (SQLException e) {
            return false;
        }
    }

    /**
     * Streams the 20 newest feed posts directly into the ByteBuf without allocating
     * Post records, ArrayList, or intermediate Strings.
     */
    public void writeFeedJson(ByteBuf buf) throws SQLException {
        buf.writeBytes(FEED_WRAPPER_PREFIX);
        boolean first = true;
        try (ResultSet rs = feedStmt.executeQuery()) {
            while (rs.next()) {
                if (!first) {
                    buf.writeByte(',');
                } else {
                    first = false;
                }
                FastJsonWriter.writePostObject(
                        buf,
                        rs.getLong(1),
                        rs.getString(2),
                        rs.getString(3),
                        rs.getString(4),
                        rs.getInt(5)
                );
            }
        }
        buf.writeByte(']');
        buf.writeByte('}');
    }

    /**
     * Streams a single post directly into the ByteBuf.
     * Returns true if post was found, false if 404.
     */
    public boolean writePostByIdJson(ByteBuf buf, long id) throws SQLException {
        postByIdStmt.setLong(1, id);
        try (ResultSet rs = postByIdStmt.executeQuery()) {
            if (!rs.next()) {
                return false;
            }
            buf.writeBytes(POST_WRAPPER_PREFIX);
            FastJsonWriter.writePostObject(
                    buf,
                    rs.getLong(1),
                    rs.getString(2),
                    rs.getString(3),
                    rs.getString(4),
                    rs.getInt(5)
            );
            buf.writeByte('}');
            return true;
        }
    }

    public List<Post> getFeed() throws SQLException {
        List<Post> posts = new ArrayList<>(20);
        try (ResultSet rs = feedStmt.executeQuery()) {
            while (rs.next()) {
                posts.add(new Post(
                        rs.getLong(1),
                        rs.getString(2),
                        rs.getString(3),
                        rs.getString(4),
                        rs.getInt(5)
                ));
            }
        }
        return posts;
    }

    public Post getPostById(long id) throws SQLException {
        postByIdStmt.setLong(1, id);
        try (ResultSet rs = postByIdStmt.executeQuery()) {
            if (rs.next()) {
                return new Post(
                        rs.getLong(1),
                        rs.getString(2),
                        rs.getString(3),
                        rs.getString(4),
                        rs.getInt(5)
                );
            }
            return null;
        }
    }

    public InsertPostResult createPost(long userId, String body) throws SQLException {
        insertPostStmt.setLong(1, userId);
        insertPostStmt.setString(2, body);
        try (ResultSet rs = insertPostStmt.executeQuery()) {
            if (rs.next()) {
                return new InsertPostResult(rs.getLong(1), rs.getString(2));
            }
            throw new SQLException("Failed to retrieve created post from RETURNING clause");
        }
    }

    public LikeResult likePost(long userId, long postId) throws SQLException {
        insertLikeStmt.setLong(1, userId);
        insertLikeStmt.setLong(2, postId);
        int rows = insertLikeStmt.executeUpdate();
        if (rows == 1) {
            return LikeResult.CREATED;
        }
        postExistsStmt.setLong(1, postId);
        try (ResultSet rs = postExistsStmt.executeQuery()) {
            if (rs.next()) {
                return LikeResult.ALREADY_LIKED;
            } else {
                return LikeResult.NOT_FOUND;
            }
        }
    }

    @Override
    public void close() {
        try { if (healthStmt != null) healthStmt.close(); } catch (Exception ignored) {}
        try { if (feedStmt != null) feedStmt.close(); } catch (Exception ignored) {}
        try { if (postByIdStmt != null) postByIdStmt.close(); } catch (Exception ignored) {}
        try { if (insertPostStmt != null) insertPostStmt.close(); } catch (Exception ignored) {}
        try { if (insertLikeStmt != null) insertLikeStmt.close(); } catch (Exception ignored) {}
        try { if (postExistsStmt != null) postExistsStmt.close(); } catch (Exception ignored) {}
        try { if (connRead != null) connRead.close(); } catch (Exception ignored) {}
        try { if (connWrite != null) connWrite.close(); } catch (Exception ignored) {}
    }
}
