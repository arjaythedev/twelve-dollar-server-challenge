package challenge.feed.config

import org.springframework.boot.context.properties.ConfigurationProperties

/**
 * Bound through setters (JavaBean style) rather than a data-class constructor: Boot's constructor binding of
 * Kotlin classes needs kotlin-reflect, which this app leaves out (see build.gradle.kts).
 */
@ConfigurationProperties("feed")
class FeedProperties {
    /** The SQLite database file (`SQLITE_PATH`). */
    lateinit var sqlitePath: String

    /** HS256 key for bearer tokens (`JWT_SECRET`). */
    lateinit var jwtSecret: String

    /** Read-only connections; reads never block on the single writer in WAL mode. */
    var readerPoolSize: Int = 4

    /** SQLite memory-maps this much of the file, so readers share the OS page cache instead of private caches. */
    var mmapSizeBytes: Long = 536_870_912
}
