package challenge;

import com.zaxxer.hikari.HikariConfig;
import com.zaxxer.hikari.HikariDataSource;
import java.nio.file.Files;
import java.nio.file.Path;
import javax.sql.DataSource;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import org.sqlite.SQLiteConfig;
import org.sqlite.SQLiteDataSource;

@SpringBootApplication
public class Application {
    public static void main(String[] args) {
        SpringApplication.run(Application.class, args);
    }

    @Bean
    DataSource dataSource(@Value("${SQLITE_PATH}") String path) {
        if (!Files.isRegularFile(Path.of(path))) {
            throw new IllegalArgumentException("SQLITE_PATH must name an existing database file");
        }
        SQLiteConfig sqlite = new SQLiteConfig();
        sqlite.setJournalMode(SQLiteConfig.JournalMode.WAL);
        sqlite.setSynchronous(SQLiteConfig.SynchronousMode.NORMAL);
        sqlite.enforceForeignKeys(true);
        sqlite.setBusyTimeout(5000);
        SQLiteDataSource source = new SQLiteDataSource(sqlite);
        source.setUrl("jdbc:sqlite:" + Path.of(path).toAbsolutePath());

        HikariConfig pool = new HikariConfig();
        pool.setDataSource(source);
        pool.setMaximumPoolSize(4);
        pool.setMinimumIdle(1);
        pool.setAutoCommit(true);
        pool.setConnectionTimeout(10000);
        return new HikariDataSource(pool);
    }
}
