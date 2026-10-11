package tds.challenge.db;

import tds.challenge.web.Routes;
import com.zaxxer.hikari.HikariConfig;
import com.zaxxer.hikari.HikariDataSource;
import io.vertx.core.json.JsonObject;
import io.vertx.ext.web.RoutingContext;

import javax.sql.DataSource;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.*;

/**
 * Owns the HikariCP pool for the SQLite database and runs schema migrations.
 */
public final class Database implements AutoCloseable {

    public static final int DB_POOL_SIZE = 4;

    private final HikariDataSource dataSource;
    private final long startMs;

    private Database(HikariDataSource dataSource) {
        this.dataSource = dataSource;
        this.startMs = System.currentTimeMillis();
    }

    public static Database create(final String sqlitePath) throws IOException {
        Path dbFile = Path.of(sqlitePath).toAbsolutePath();
        if (dbFile.getParent() != null) {
            Files.createDirectories(dbFile.getParent());
        }

        HikariConfig hc = new HikariConfig();
        hc.setPoolName("sqlite-pool");
        hc.setJdbcUrl("jdbc:sqlite:" + dbFile);
        hc.setMaximumPoolSize(DB_POOL_SIZE);
        hc.setMinimumIdle(1);
        hc.setConnectionTimeout(10_000);

        // sqlite-jdbc driver properties (per connection)
        hc.addDataSourceProperty("journal_mode", "WAL");
        hc.addDataSourceProperty("busy_timeout", "500");
        hc.addDataSourceProperty("foreign_keys", "true");
        hc.addDataSourceProperty("synchronous", "NORMAL");

        return new Database(new HikariDataSource(hc));
    }

    public DataSource dataSource() {
        return dataSource;
    }

    @Override
    public void close() {
        dataSource.close();
    }

    public void verityConnection(RoutingContext ctx) {
        try (Connection c = dataSource.getConnection()) {
            PreparedStatement ps = c.prepareStatement("SELECT 1;");
            ps.executeQuery();
            final long uptimeS = (System.currentTimeMillis() - this.startMs) / 1000;
            Routes.json(ctx, 200, new JsonObject().put("status", "ok").put("db", "ok").put("uptime_s", uptimeS));
        } catch (SQLException sqle) {
            Routes.json(ctx, 200, new JsonObject().put("status", "degraded").put("db", "unreachable").put("error", sqle.getMessage()));
        }
    }
}
