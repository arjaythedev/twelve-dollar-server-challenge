package io.github.wolches.challenge12.dao;

import io.agroal.api.AgroalDataSource;
import io.github.wolches.challenge12.exception.InternalServerErrorException;
import io.github.wolches.challenge12.exception.PostNotFoundException;
import io.github.wolches.challenge12.model.PostDto;
import io.github.wolches.challenge12.model.persistent.PostResult;
import io.quarkus.agroal.DataSource;
import io.smallrye.mutiny.Uni;
import io.vertx.mutiny.core.Vertx;
import jakarta.annotation.PreDestroy;
import jakarta.enterprise.context.ApplicationScoped;
import jakarta.inject.Inject;
import org.sqlite.SQLiteException;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.ArrayList;
import java.util.List;

import static org.sqlite.SQLiteErrorCode.SQLITE_CONSTRAINT_FOREIGNKEY;

@ApplicationScoped
public class AsyncSeparatedReadWriteDao implements AutoCloseable {

    public static final String UPSERT_POST = """
            INSERT INTO posts (user_id, body)
            VALUES (?, ?)
            RETURNING id, created_at;
            """;
    public static final String UPSERT_LIKE = """
            INSERT INTO likes (user_id, post_id)
            VALUES (?, ?)
            ON CONFLICT (user_id, post_id) DO NOTHING;
            """;
    public static final String SELECT_FEED = """
            SELECT
                p.id,
                p.body,
                p.created_at,
                u.username,
                (
                    SELECT count(*)
                    FROM likes l
                    WHERE l.post_id = p.id
                ) AS like_count
            FROM posts p
            JOIN users u ON u.id = p.user_id
            ORDER BY
                p.created_at DESC,
                p.id DESC
            LIMIT 20;
            """;

    public static final String SELECT_POST_BY_ID = """
            SELECT
                p.id,
                p.body,
                p.created_at,
                u.username,
                (
                    SELECT count(*)
                    FROM likes l
                    WHERE l.post_id = p.id
                ) AS like_count
            FROM posts p
            JOIN users u ON u.id = p.user_id
            WHERE p.id = ?
            """;

    public static final String HEALTH_CHECK = "SELECT 1;";

    @Inject
    public AsyncSeparatedReadWriteDao(AgroalDataSource writer,
                                      @DataSource("readers") AgroalDataSource readers,
                                      Vertx vertx
    ) throws SQLException {
        this.reader = new AsyncDbWorker(readers, vertx, "reader-worker");
        this.writer = new AsyncDbWorker(writer, vertx, "writer-worker");
    }

    private final AsyncDbWorker reader;
    private final AsyncDbWorker writer;

    public Uni<Boolean> like(int userId, int postId) {
        return writer.tx(conn -> {
                try {
                    var statement = writer.prepareStatement(UPSERT_LIKE);
                    statement.setInt(1, userId);
                    statement.setInt(2, postId);
                    statement.execute();
                    return statement.getUpdateCount() == 1;
                } catch (SQLiteException e) {
                    if (e.getResultCode() == SQLITE_CONSTRAINT_FOREIGNKEY) {
                        throw new PostNotFoundException();
                    } else {
                        throw new InternalServerErrorException();
                    }
                }
            });
    }

    public Uni<PostResult> post(int userId, String body) {
        return writer.tx(conn -> {
                var statement = writer.prepareStatement(UPSERT_POST);
                statement.setInt(1, userId);
                statement.setString(2, body);
                statement.execute();
                try (var resultSet = statement.getResultSet()) {
                    resultSet.next();
                    return PostResult.builder()
                            .id(resultSet.getInt(1))
                            .createdAt(resultSet.getString(2))
                            .build();
                }
            });
    }

    public Uni<PostDto> getPostById(int postId) {
        return reader.tx(conn -> {
            var statement = reader.prepareStatement(SELECT_POST_BY_ID);
            statement.setInt(1, postId);
            statement.execute();
            try (var resultSet = statement.getResultSet()) {
                var hasResult = resultSet.next();
                if (hasResult) return extractResultSet(resultSet);
                else throw new PostNotFoundException();
            }
        });
    }

    public Uni<List<PostDto>> getFeed() {
        return reader.tx(conn -> {
            var statement = reader.prepareStatement(SELECT_FEED);
            statement.execute();
            try (var resultSet = statement.getResultSet()) {
                var posts = new ArrayList<PostDto>(20);
                while (resultSet.next()) {
                    posts.add(extractResultSet(resultSet));
                }
                return posts;
            }
        });
    }

    public Uni<Boolean> healthCheck() {
        return reader.tx(conn -> {
            return reader.prepareStatement(HEALTH_CHECK).execute();
        });
    }

    private PostDto extractResultSet(ResultSet rs) throws SQLException {
        return PostDto.builder()
                .id(rs.getInt(1))
                .body(rs.getString(2))
                .createdAt(rs.getString(3))
                .author(rs.getString(4))
                .likeCount(rs.getInt(5))
                .build();
    }

    @Override
    @PreDestroy
    public void close() throws Exception {
        reader.close();
        writer.close();
    }
}
