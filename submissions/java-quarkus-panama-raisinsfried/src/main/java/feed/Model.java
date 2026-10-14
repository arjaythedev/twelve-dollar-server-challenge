package feed;

import com.fasterxml.jackson.annotation.JsonProperty;
import com.fasterxml.jackson.annotation.JsonPropertyOrder;
import java.util.List;

/** The JSON bodies, as records Jackson writes, with SPEC.md's key order. */
public final class Model {
    private Model() {}

    @JsonPropertyOrder({"id", "body", "created_at", "author", "like_count"})
    public record Post(long id, String body, @JsonProperty("created_at") String createdAt, String author,
            @JsonProperty("like_count") long likeCount) {}

    public record PostBody(Post post) {}

    public record Feed(List<Post> posts) {}

    @JsonPropertyOrder({"liked", "already_liked", "post_id"})
    public record Liked(boolean liked, @JsonProperty("already_liked") boolean alreadyLiked,
            @JsonProperty("post_id") long postId) {}

    @JsonPropertyOrder({"status", "db", "uptime_s"})
    public record Health(String status, String db, @JsonProperty("uptime_s") long uptimeS) {}

    @JsonPropertyOrder({"status", "db", "error"})
    public record Degraded(String status, String db, String error) {}

    public record ErrorBody(String error) {}
}
