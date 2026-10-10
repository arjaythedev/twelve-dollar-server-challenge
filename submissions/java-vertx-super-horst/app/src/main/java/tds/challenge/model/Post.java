package tds.challenge.model;

import io.vertx.core.json.JsonObject;

public record Post(long id, String body, String createdAt, String user, long likeCount) {

    public JsonObject toJson() {
        return new JsonObject()
                .put("id", id)
                .put("body", body)
                .put("created_at", createdAt)
                .put("author", user)
                .put("like_count", likeCount);
    }
}
