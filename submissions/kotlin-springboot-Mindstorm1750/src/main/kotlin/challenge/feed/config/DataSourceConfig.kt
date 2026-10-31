package challenge.feed.config

import com.zaxxer.hikari.HikariConfig
import com.zaxxer.hikari.HikariDataSource
import org.springframework.beans.factory.annotation.Qualifier
import org.springframework.context.annotation.Bean
import org.springframework.context.annotation.Configuration
import org.springframework.jdbc.core.simple.JdbcClient
import org.sqlite.SQLiteConfig
import org.sqlite.SQLiteDataSource
import javax.sql.DataSource

/**
 * Two pools over the same SQLite file: read-only readers, and exactly one writer so writes queue in the JVM
 * instead of failing with SQLITE_BUSY. Both run in autocommit, so each write is committed before we respond.
 */
@Configuration(proxyBeanMethods = false)
class DataSourceConfig(private val props: FeedProperties) {

    @Bean(destroyMethod = "close")
    fun readerDataSource(): HikariDataSource = pool("reader", props.readerPoolSize, readOnly = true)

    @Bean(destroyMethod = "close")
    fun writerDataSource(): HikariDataSource = pool("writer", 1, readOnly = false)

    @Bean
    fun readerJdbc(@Qualifier("readerDataSource") dataSource: DataSource): JdbcClient = JdbcClient.create(dataSource)

    @Bean
    fun writerJdbc(@Qualifier("writerDataSource") dataSource: DataSource): JdbcClient = JdbcClient.create(dataSource)

    private fun pool(name: String, size: Int, readOnly: Boolean): HikariDataSource {
        val sqlite = SQLiteConfig().apply {
            setReadOnly(readOnly)
            if (!readOnly) setJournalMode(SQLiteConfig.JournalMode.WAL)
            setSynchronous(SQLiteConfig.SynchronousMode.NORMAL)
            setBusyTimeout(5_000)
            setTempStore(SQLiteConfig.TempStore.MEMORY)
            setCacheSize(-8_000) // negative = KiB; small, because mmap serves most reads
            setPragma(SQLiteConfig.Pragma.MMAP_SIZE, props.mmapSizeBytes.toString())
            // Otherwise the driver runs an extra SELECT last_insert_rowid() after every insert; posts come back
            // through RETURNING and likes need no key.
            setGetGeneratedKeys(false)
        }
        val dataSource = SQLiteDataSource(sqlite).apply { url = "jdbc:sqlite:${props.sqlitePath}" }
        return HikariDataSource(HikariConfig().apply {
            poolName = name
            this.dataSource = StatementCachingDataSource(dataSource)
            maximumPoolSize = size
            minimumIdle = size
            isReadOnly = readOnly
        })
    }
}
