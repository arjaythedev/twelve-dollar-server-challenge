package dev.relism.benchmark;

import dev.relism.benchmark.auth.Tokens;
import dev.relism.benchmark.db.Database;
import dev.relism.benchmark.http.Json;
import dev.relism.flash.App;
import dev.relism.flash.Module;
import dev.relism.flash.http.ContentType;
import dev.relism.flash.http.HttpException;

public final class FeedApp implements Module {
    private final String database;
    private final String secret;

    public FeedApp(String database, String secret) {
        this.database = database;
        this.secret = secret;
    }

    @Override public void install(App app) {
        var tokens = new Tokens(secret);
        app.add(Tokens.class, tokens)
                .add(Database.class, new Database(database, tokens))
                .scan("dev.relism.benchmark.api")
                .onNotFound((req, res) -> res.status(404).type(ContentType.JSON)
                        .body(Json.error("not found")))
                .onError((failure, req, res) -> {
                    int status = failure instanceof HttpException e ? e.status() : 500;
                    String message = failure instanceof HttpException e ? e.getMessage() : "internal server error";
                    return res.status(status).type(ContentType.JSON).body(Json.error(message));
                });
    }
}
