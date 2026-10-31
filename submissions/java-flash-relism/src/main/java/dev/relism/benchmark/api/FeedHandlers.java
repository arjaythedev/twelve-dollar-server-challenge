package dev.relism.benchmark.api;

import dev.relism.benchmark.db.Database;
import dev.relism.flash.http.ContentType;
import dev.relism.flash.http.Request;
import dev.relism.flash.http.RequestHandler;
import dev.relism.flash.http.Response;
import dev.relism.flash.routing.GET;
import dev.relism.flash.routing.POST;
import static dev.relism.benchmark.db.Database.Operation.*;

public final class FeedHandlers {
    private FeedHandlers() {}

    @GET("/health")
    public static final class Health extends RequestHandler {
        private final Database db;
        public Health(Database db) { this.db = db; }
        @Override protected Object handle(Request req, Response res) {
            return respond(res, db.execute(HEALTH, null, null, null));
        }
    }

    @GET("/feed")
    public static final class Feed extends RequestHandler {
        private final Database db;
        public Feed(Database db) { this.db = db; }
        @Override protected Object handle(Request req, Response res) {
            return respond(res, db.execute(FEED, null, null, null));
        }
    }

    @GET("/posts/{id}")
    public static final class Get extends RequestHandler {
        private final Database db;
        public Get(Database db) { this.db = db; }
        @Override protected Object handle(Request req, Response res) {
            return respond(res, db.execute(GET, req.param("id"), null, null));
        }
    }

    @POST("/posts")
    public static final class Create extends RequestHandler {
        private final Database db;
        public Create(Database db) { this.db = db; }
        @Override protected Object handle(Request req, Response res) throws Exception {
            return respond(res, db.execute(CREATE, null, req.headerView("Authorization"), req.body().bytes()));
        }
    }

    @POST("/posts/{id}/like")
    public static final class Like extends RequestHandler {
        private final Database db;
        public Like(Database db) { this.db = db; }
        @Override protected Object handle(Request req, Response res) {
            return respond(res, db.execute(LIKE, req.param("id"), req.headerView("Authorization"), null));
        }
    }

    private static Response respond(Response res, Database.Reply reply) {
        return res.status(reply.status()).type(ContentType.JSON).body(reply.body());
    }
}
