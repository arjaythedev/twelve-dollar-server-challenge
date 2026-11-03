package com.dglynch.twelvedollar;

import module java.base;
import module java.sql;
import org.sqlite.SQLiteConfig;
import org.sqlite.SQLiteConfig.JournalMode;
import org.sqlite.SQLiteConfig.SynchronousMode;
import org.sqlite.SQLiteConfig.TempStore;

public class DatabaseModel {
    private static final LikeResult NEWLY_LIKED = new LikeResult.NewlyLiked();
    private static final LikeResult ALREADY_LIKED = new LikeResult.AlreadyLiked();
    private static final LikeResult POST_NOT_FOUND = new LikeResult.PostNotFound();

    private final Connection writeConnection;

    private final PreparedStatement healthStatement;
    private final PreparedStatement feedStatement;
    private final PreparedStatement getPostStatement;
    private final PreparedStatement createPostStatement;
    private final PreparedStatement likePostStatement;
    private final PreparedStatement createSavepointStatement;
    private final PreparedStatement releaseSavepointStatement;
    private final PreparedStatement rollbackToSavepointStatement;

    private final ExecutorService readExecutor = Executors.newSingleThreadExecutor(Thread.ofPlatform().name("database-read").factory());
    private final ExecutorService writeExecutor = Executors.newSingleThreadExecutor(Thread.ofPlatform().name("database-write").factory());

    private final BlockingQueue<WriteOperation> writeQueue = new LinkedBlockingQueue<>();

    public DatabaseModel(String url) throws SQLException {
        var config = new SQLiteConfig();
        config.setJournalMode(JournalMode.WAL);
        config.setSynchronous(SynchronousMode.NORMAL);
        config.setCacheSize(-65_536);
        config.setTempStore(TempStore.MEMORY);
        config.setPragma(SQLiteConfig.Pragma.FOREIGN_KEYS, "ON");

        var readConnection = DriverManager.getConnection(url, config.toProperties());
        writeConnection = DriverManager.getConnection(url, config.toProperties());
        writeConnection.setAutoCommit(false);

        healthStatement = readConnection.prepareStatement("SELECT 1");
        feedStatement = readConnection.prepareStatement(
                "SELECT p.id, p.body, p.created_at, u.username, " +
                        "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count " +
                        "FROM posts p JOIN users u ON u.id = p.user_id " +
                        "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
        getPostStatement = readConnection.prepareStatement(
                "SELECT p.id, p.body, p.created_at, u.username, " +
                        "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count " +
                        "FROM posts p JOIN users u ON u.id = p.user_id " +
                        "WHERE p.id = ?");
        createPostStatement = writeConnection.prepareStatement(
                "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
                Statement.RETURN_GENERATED_KEYS);
        likePostStatement = writeConnection.prepareStatement(
                "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING");
        createSavepointStatement = writeConnection.prepareStatement("SAVEPOINT batch_savepoint");
        rollbackToSavepointStatement = writeConnection.prepareStatement("ROLLBACK TRANSACTION TO SAVEPOINT batch_savepoint");
        releaseSavepointStatement = writeConnection.prepareStatement("RELEASE SAVEPOINT batch_savepoint");

        writeExecutor.submit(this::writeExecutorLoop);
    }

    public boolean isHealthy() {
        try {
            return readExecutor.submit(() -> {
                try (var resultSet = healthStatement.executeQuery()) {
                    return resultSet.next();
                }
            }).get();
        } catch (Exception e) {
            e.printStackTrace();
            return false;
        }
    }

    public List<Post> getFeed() {
        try {
            return readExecutor.submit(() -> {
                try (var resultSet = feedStatement.executeQuery()) {
                    var posts = new ArrayList<Post>();
                    while (resultSet.next()) {
                        posts.add(new Post(
                                resultSet.getLong("id"),
                                resultSet.getString("body"),
                                resultSet.getString("created_at"),
                                resultSet.getString("username"),
                                resultSet.getLong("like_count")));
                    }
                    return posts;
                }
            }).get();
        } catch (Exception e) {
            e.printStackTrace();
            return List.of();
        }
    }

    public Post getPost(long id) {
        try {
            return readExecutor.submit(() -> {
                getPostStatement.setLong(1, id);
                try (var resultSet = getPostStatement.executeQuery()) {
                    if (resultSet.next()) {
                        return new Post(
                                resultSet.getLong("id"),
                                resultSet.getString("body"),
                                resultSet.getString("created_at"),
                                resultSet.getString("username"),
                                resultSet.getLong("like_count"));
                    } else {
                        return null;
                    }
                }
            }).get();
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    public Post createPost(long userId, String author, String body) {
        try {
            var future = new CompletableFuture<Post>();
            if (writeQueue.offer(new WriteOperation.PostOperation(userId, body, author, future))) {
                return future.get();
            } else {
                return null;
            }
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    public LikeResult likePost(long postId, long userId) {
        try {
            var future = new CompletableFuture<LikeResult>();
            if (writeQueue.offer(new WriteOperation.LikeOperation(postId, userId, future))) {
                return future.get();
            } else {
                return null;
            }
        } catch (Exception e) {
            e.printStackTrace();
            return null;
        }
    }

    private void writeExecutorLoop() {
        var batchOperations = new ArrayList<WriteOperation>();
        var batchPostResults = new ArrayDeque<Post>();
        var batchLikeResults = new ArrayDeque<LikeResult>();

        while (!Thread.currentThread().isInterrupted()) {
            try {
                var firstOperation = writeQueue.take();
                batchOperations.add(firstOperation);
                writeQueue.drainTo(batchOperations);

                for (var operation : batchOperations) {
                    switch (operation) {
                        case WriteOperation.PostOperation postOperation ->
                                batchPostResults.offer(processPostOperation(postOperation));
                        case WriteOperation.LikeOperation likeOperation ->
                                batchLikeResults.offer(processLikeOperation(likeOperation));
                    }
                }
                writeConnection.commit();

                for (var operation : batchOperations) {
                    switch (operation) {
                        case WriteOperation.PostOperation postOperation ->
                                postOperation.future().complete(batchPostResults.poll());
                        case WriteOperation.LikeOperation likeOperation ->
                                likeOperation.future().complete(batchLikeResults.poll());
                    }
                }
                batchOperations.clear();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            } catch (SQLException e) {
                e.printStackTrace();
                try {
                    writeConnection.rollback();
                } catch (SQLException rollbackEx) {
                    rollbackEx.printStackTrace();
                }
            }
        }
    }

    private Post processPostOperation(WriteOperation.PostOperation postOperation) throws SQLException {
        createPostStatement.setLong(1, postOperation.userId());
        createPostStatement.setString(2, postOperation.body());
        try (var resultSet = createPostStatement.executeQuery()) {
            if (resultSet.next()) {
                return new Post(
                        resultSet.getLong("id"),
                        postOperation.body(),
                        resultSet.getString("created_at"),
                        postOperation.author(),
                        0);
            } else {
                throw new SQLException("Failed to create post");
            }
        }
    }

    private LikeResult processLikeOperation(WriteOperation.LikeOperation likeOperation) throws SQLException {
        likePostStatement.setLong(1, likeOperation.userId());
        likePostStatement.setLong(2, likeOperation.postId());
        try {
            createSavepointStatement.execute();
            var rowCount = likePostStatement.executeUpdate();
            releaseSavepointStatement.execute();
            if (rowCount > 0) {
                return NEWLY_LIKED;
            } else {
                return ALREADY_LIKED;
            }
        } catch (SQLException e) {
            if (e.getErrorCode() == 19) {
                rollbackToSavepointStatement.execute();
                return POST_NOT_FOUND;
            } else {
                throw e;
            }
        }
    }

    private sealed interface WriteOperation {
        record PostOperation(long userId, String body, String author,
                             CompletableFuture<Post> future) implements WriteOperation {
        }

        record LikeOperation(long postId, long userId, CompletableFuture<LikeResult> future) implements WriteOperation {
        }
    }
}
