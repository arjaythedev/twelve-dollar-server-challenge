package com.brocchirodrigo.feed.infrastructure.persistence.repository;

import com.brocchirodrigo.feed.domain.model.PersistedPost;
import com.brocchirodrigo.feed.domain.model.Post;
import com.brocchirodrigo.feed.domain.repository.PostRepository;
import com.brocchirodrigo.feed.infrastructure.persistence.mapper.PostRowMapper;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteConnection;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteDatabase;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteException;
import com.brocchirodrigo.feed.infrastructure.persistence.sqlite.SqliteStatement;

import java.lang.foreign.Arena;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;

public final class SqlitePostRepository implements PostRepository, AutoCloseable {

    private static final String SELECT_POST = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
              FROM posts p JOIN users u ON u.id = p.user_id
            """;

    private final Object lock = new Object();
    private final SqliteConnection connection;
    private final SqliteStatement latestStatement;
    private final SqliteStatement byIdStatement;
    private final SqliteStatement insertStatement;
    private final SqliteStatement likeStatement;
    private final SqliteStatement existsStatement;

    public SqlitePostRepository(SqliteDatabase database) {
        this.connection = database.open();
        this.latestStatement = connection.prepare(SELECT_POST + "ORDER BY p.created_at DESC, p.id DESC LIMIT ?");
        this.byIdStatement = connection.prepare(SELECT_POST + "WHERE p.id = ?");
        this.insertStatement = connection.prepare("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
        this.likeStatement = connection.prepare("""
                INSERT INTO likes (user_id, post_id)
                SELECT ?, id FROM posts WHERE id = ?
                ON CONFLICT DO NOTHING
                """);
        this.existsStatement = connection.prepare("SELECT 1 FROM posts WHERE id = ?");
    }

    @Override
    public List<Post> latest(int limit) {
        synchronized (lock) {
            try {
                latestStatement.bindLong(1, limit);
                var posts = new ArrayList<Post>(limit);
                while (latestStatement.step()) {
                    posts.add(PostRowMapper.toPost(latestStatement));
                }
                return posts;
            } finally {
                latestStatement.reset();
            }
        }
    }

    @Override
    public Optional<Post> findById(long id) {
        synchronized (lock) {
            try {
                byIdStatement.bindLong(1, id);
                return byIdStatement.step() ? Optional.of(PostRowMapper.toPost(byIdStatement)) : Optional.empty();
            } finally {
                byIdStatement.reset();
            }
        }
    }

    @Override
    public PersistedPost create(long authorId, String body) {
        synchronized (lock) {
            try (var arena = Arena.ofConfined()) {
                try {
                    insertStatement.bindLong(1, authorId);
                    insertStatement.bindText(2, body, arena);
                    if (!insertStatement.step()) {
                        throw new SqliteException("post insert returned no row");
                    }
                    var persisted = new PersistedPost(insertStatement.getLong(0), insertStatement.getText(1));
                    insertStatement.step();
                    return persisted;
                } finally {
                    insertStatement.reset();
                }
            }
        }
    }

    @Override
    public boolean addLike(long userId, long postId) {
        synchronized (lock) {
            try {
                likeStatement.bindLong(1, userId);
                likeStatement.bindLong(2, postId);
                likeStatement.step();
                return connection.changes() == 1;
            } finally {
                likeStatement.reset();
            }
        }
    }

    @Override
    public boolean exists(long id) {
        synchronized (lock) {
            try {
                existsStatement.bindLong(1, id);
                return existsStatement.step();
            } finally {
                existsStatement.reset();
            }
        }
    }

    @Override
    public void close() {
        synchronized (lock) {
            latestStatement.close();
            byIdStatement.close();
            insertStatement.close();
            likeStatement.close();
            existsStatement.close();
            connection.close();
        }
    }
}
