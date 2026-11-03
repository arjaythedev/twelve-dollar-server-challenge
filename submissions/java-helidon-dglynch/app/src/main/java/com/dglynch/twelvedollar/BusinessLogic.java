package com.dglynch.twelvedollar;

import module java.base;

public class BusinessLogic {
    private static final HealthStatus DATABASE_FAILURE = new HealthStatus.DatabaseFailure();
    private static final HealthStatus NOT_STARTED = new HealthStatus.NotStarted();

    private final DatabaseModel databaseModel;

    private boolean started = false;
    private long startTime;

    public BusinessLogic(DatabaseModel databaseModel) {
        this.databaseModel = databaseModel;
    }

    public void start() {
        startTime = System.nanoTime();
        started = true;
    }

    public HealthStatus health() {
        if (started) {
            if (databaseModel.isHealthy()) {
                var uptime = TimeUnit.NANOSECONDS.toSeconds(System.nanoTime() - startTime);
                return new HealthStatus.Ok(uptime);
            } else {
                return DATABASE_FAILURE;
            }
        } else {
            return NOT_STARTED;
        }
    }

    public List<Post> feed() {
        return databaseModel.getFeed();
    }

    public Post getPost(long id) {
        return databaseModel.getPost(id);
    }

    public Post createPost(long userId, String author, String body) {
        return databaseModel.createPost(userId, author, body);
    }

    public LikeResult likePost(long postId, long userId) {
        return databaseModel.likePost(postId, userId);
    }
}
