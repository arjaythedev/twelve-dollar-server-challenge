package challenge;

import com.fasterxml.jackson.annotation.JsonPropertyOrder;

final class Responses {
    private Responses() {}

    @JsonPropertyOrder({"id", "body", "created_at", "author", "like_count"})
    record Post(long id, String body, String created_at, String author, long like_count) {}

    @JsonPropertyOrder({"liked", "already_liked", "post_id"})
    record Like(boolean liked, boolean already_liked, long post_id) {}

    @JsonPropertyOrder({"status", "db", "uptime_s"})
    record Health(String status, String db, long uptime_s) {}

    @JsonPropertyOrder({"status", "db", "error"})
    record DegradedHealth(String status, String db, String error) {}

    record Error(String error) {}
}
