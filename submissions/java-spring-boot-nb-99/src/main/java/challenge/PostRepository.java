package challenge;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.List;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;

@Repository
class PostRepository {
    private static final String SELECT_POST =
            """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;
    private final JdbcTemplate jdbc;

    PostRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    void checkHealth() {
        jdbc.queryForObject("SELECT 1", Integer.class);
    }

    List<Responses.Post> feed() {
        return jdbc.query(
                SELECT_POST + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", this::post);
    }

    Responses.Post get(long id) {
        List<Responses.Post> posts = jdbc.query(SELECT_POST + " WHERE p.id = ?", this::post, id);
        if (posts.isEmpty()) {
            throw new ApiException(404, "post not found");
        }
        return posts.getFirst();
    }

    Responses.Post create(Authentication.User user, String body) {
        // JdbcTemplate closes the RETURNING result and statement before returning, completing
        // auto-commit.
        return jdbc.queryForObject(
                "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
                (row, index) ->
                        new Responses.Post(
                                row.getLong("id"),
                                body,
                                row.getString("created_at"),
                                user.username(),
                                0),
                user.id(),
                body);
    }

    boolean like(long userId, long postId) {
        if (jdbc.queryForObject("SELECT count(*) FROM posts WHERE id = ?", Integer.class, postId)
                == 0) {
            throw new ApiException(404, "post not found");
        }
        return jdbc.update(
                        """
                INSERT INTO likes (user_id, post_id) VALUES (?, ?)
                ON CONFLICT (user_id, post_id) DO NOTHING
                """,
                        userId,
                        postId)
                == 0;
    }

    private Responses.Post post(ResultSet row, int index) throws SQLException {
        return new Responses.Post(
                row.getLong("id"),
                row.getString("body"),
                row.getString("created_at"),
                row.getString("username"),
                row.getLong("like_count"));
    }
}
