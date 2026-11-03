package feed;

import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.ObjectReader;
import feed.Model.Degraded;
import feed.Model.Feed;
import feed.Model.Health;
import feed.Model.Liked;
import feed.Model.Post;
import feed.Model.PostBody;
import feed.Sqlite.SqliteException;
import io.quarkus.logging.Log;
import io.smallrye.common.annotation.NonBlocking;
import io.smallrye.mutiny.Uni;
import jakarta.ws.rs.GET;
import jakarta.ws.rs.POST;
import jakarta.ws.rs.Path;
import jakarta.ws.rs.Produces;
import jakarta.ws.rs.core.MediaType;
import java.io.IOException;
import java.lang.management.ManagementFactory;
import org.jboss.resteasy.reactive.RestHeader;
import org.jboss.resteasy.reactive.RestPath;
import org.jboss.resteasy.reactive.RestResponse;
import org.jboss.resteasy.reactive.RestResponse.Status;

/** The five endpoints of SPEC.md, run on the event loop; writes complete after their group commit. */
@Path("/")
@Produces(MediaType.APPLICATION_JSON)
@NonBlocking
public class FeedResource {
    private static final int MAX_BODY_CHARS = 500;

    private final Database db;
    private final Jwt jwt;
    /** Strict about trailing content: {@code {"body":"x"} junk} is malformed JSON. */
    private final ObjectReader json;

    FeedResource(Database db, Jwt jwt, ObjectMapper mapper) {
        this.db = db;
        this.jwt = jwt;
        this.json = mapper.reader().with(DeserializationFeature.FAIL_ON_TRAILING_TOKENS);
    }

    @GET
    @Path("health")
    public RestResponse<Object> health() {
        try {
            db.read(conn -> {
                conn.ping();
                return null;
            });
            return RestResponse.ok(new Health("ok", "ok", ManagementFactory.getRuntimeMXBean().getUptime() / 1000));
        } catch (SqliteException e) {
            Log.warnf("health check failed: %s", e.getMessage());
            return RestResponse.status(Status.SERVICE_UNAVAILABLE, new Degraded("degraded", "unreachable", "database error"));
        }
    }

    @GET
    @Path("feed")
    public Feed feed() {
        return new Feed(db.read(NativeDb::feed));
    }

    @GET
    @Path("posts/{id}")
    public PostBody post(@RestPath String id) {
        long postId = postId(id);
        Post post = db.read(conn -> conn.post(postId));
        if (post == null) throw new ApiError(404, "post not found");
        return new PostBody(post);
    }

    /** Parsed after the token check, so a request without a token is a 401. */
    @POST
    @Path("posts")
    public Uni<RestResponse<PostBody>> create(@RestHeader("Authorization") String authorization, byte[] request) {
        Jwt.User user = jwt.verify(authorization);
        JsonNode node;
        try {
            node = json.readTree(request);
        } catch (IOException e) {
            throw new ApiError(400, "malformed JSON body");
        }
        if (node == null || node.isMissingNode()) throw new ApiError(400, "malformed JSON body"); // empty body
        JsonNode field = node.get("body");
        if (field == null || !field.isTextual()) throw new ApiError(400, "body is required");
        String body = trim(field.textValue());
        if (body.isEmpty()) throw new ApiError(400, "body is required");
        if (body.codePointCount(0, body.length()) > MAX_BODY_CHARS) {
            throw new ApiError(400, "body must be at most 500 characters");
        }
        return db.write(conn -> conn.insertPost(user.id(), body)).map(row -> RestResponse.status(Status.CREATED,
                new PostBody(new Post(row.id(), body, row.createdAt(), user.username(), 0))));
    }

    @POST
    @Path("posts/{id}/like")
    public Uni<RestResponse<Liked>> like(@RestHeader("Authorization") String authorization, @RestPath String id) {
        Jwt.User user = jwt.verify(authorization);
        long postId = postId(id);
        return db.write(conn -> conn.like(user.id(), postId)).map(result -> {
            if (result == NativeDb.Like.NO_SUCH_POST) throw new ApiError(404, "post not found");
            boolean created = result == NativeDb.Like.CREATED;
            return RestResponse.status(created ? Status.CREATED : Status.OK, new Liked(true, !created, postId));
        });
    }

    private static long postId(String id) {
        long value = positiveId(id);
        if (value < 0) throw new ApiError(400, "invalid post id");
        return value;
    }

    /** ASCII digits only, at most 18; -1 if not a positive integer. */
    static long positiveId(String s) {
        int n = s.length();
        if (n == 0 || n > 18) return -1;
        long v = 0;
        for (int i = 0; i < n; i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v > 0 ? v : -1;
    }

    /** JavaScript's trim() set, which differs from Java's strip() (U+00A0, U+FEFF in; U+001C..U+001F out). */
    static String trim(String s) {
        int start = 0, end = s.length();
        while (start < end && isJsWhitespace(s.charAt(start))) start++;
        while (end > start && isJsWhitespace(s.charAt(end - 1))) end--;
        return s.substring(start, end);
    }

    private static boolean isJsWhitespace(char c) {
        return switch (c) {
            case '\t', '\n', '\u000B', '\f', '\r', ' ', '\u00A0', '\u1680', '\u2028', '\u2029', '\u202F', '\u205F',
                 '\u3000', '\uFEFF' -> true;
            default -> c >= '\u2000' && c <= '\u200A';
        };
    }
}
