package tds.challenge;

import tds.challenge.db.Database;
import tds.challenge.repository.PostRepository;
import tds.challenge.web.ApiHandler;
import tds.challenge.web.Routes;
import io.vertx.core.AbstractVerticle;
import io.vertx.core.Handler;
import io.vertx.core.Promise;
import io.vertx.core.WorkerExecutor;
import io.vertx.ext.web.Router;
import io.vertx.ext.web.RoutingContext;

import java.util.Optional;

public class MainVerticle extends AbstractVerticle {

    private Database database;
    private WorkerExecutor dbExecutor;

    @Override
    public void start(Promise<Void> startPromise) {
        final String host = Optional.ofNullable(System.getenv("HOST")).orElseThrow(() -> new IllegalStateException("Require HOST env-var"));
        final int port = Integer.parseInt(Optional.ofNullable(System.getenv("PORT")).orElseThrow(() -> new IllegalStateException("Require PORT env-var")));
        final String sqlitePath = Optional.ofNullable(System.getenv("SQLITE_PATH")).orElseThrow(() -> new IllegalStateException("Require SQLITE_PATH env-var"));
        final String jwtSecret = Optional.ofNullable(System.getenv("JWT_SECRET")).orElseThrow(() -> new IllegalStateException("Require JWT_SECRET env-var"));

        try {
            // One-time, blocking startup work; acceptable before the server begins listening.
            database = Database.create(sqlitePath);
        } catch (Exception e) {
            startPromise.fail(e);
            return;
        }

        // dedicated worker pool same size of the Hikari pool so blocking JDBC calls
        // never run on (and never block) the event loop.
        dbExecutor = vertx.createSharedWorkerExecutor("db-worker", Database.DB_POOL_SIZE);

        // wonky lambda construction, to at least pretend there is some abstraction between db and router
        Handler<RoutingContext> dbHealthHandler = ctx -> dbExecutor.executeBlocking(() -> {
            database.verityConnection(ctx);
            return null;
        }, false);

        PostRepository postRepository = new PostRepository(database.dataSource(), dbExecutor);
        ApiHandler apiHandler = new ApiHandler(postRepository);
        Router router = Routes.build(vertx, apiHandler, dbHealthHandler, jwtSecret);

        vertx.createHttpServer().requestHandler(router).listen(port, host).onSuccess(server -> {
            System.out.printf("HTTP server listening on port %s%n", server.actualPort());
            startPromise.complete();
        }).onFailure(startPromise::fail);
    }

    @Override
    public void stop() {
        if (dbExecutor != null) {
            dbExecutor.close();
        }
        if (database != null) {
            database.close();
        }
    }
}
