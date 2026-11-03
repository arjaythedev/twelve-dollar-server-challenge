package challenge.feed.config

import challenge.feed.TestDatabase
import challenge.feed.TestTokens
import org.springframework.beans.factory.annotation.Autowired
import org.springframework.beans.factory.annotation.Qualifier
import org.springframework.boot.test.context.SpringBootTest
import org.springframework.test.context.DynamicPropertyRegistry
import org.springframework.test.context.DynamicPropertySource
import org.sqlite.SQLiteDataSource
import javax.sql.DataSource
import kotlin.test.Test
import kotlin.test.assertFalse

@SpringBootTest(webEnvironment = SpringBootTest.WebEnvironment.NONE, properties = ["feed.jwt-secret=${TestTokens.SECRET}"])
class DataSourceConfigTest {
    @Autowired @Qualifier("readerDataSource") lateinit var reader: DataSource
    @Autowired @Qualifier("writerDataSource") lateinit var writer: DataSource

    @Test
    fun `inserts do not run an extra SELECT last_insert_rowid()`() {
        // Posts come back through RETURNING and likes need no key, so the driver's generated-keys query is waste.
        for (pool in listOf(reader, writer)) {
            assertFalse(pool.unwrap(SQLiteDataSource::class.java).config.isGetGeneratedKeys)
        }
    }

    companion object {
        private val dbPath = TestDatabase.create()

        @JvmStatic
        @DynamicPropertySource
        fun properties(registry: DynamicPropertyRegistry) {
            registry.add("feed.sqlite-path") { dbPath.toString() }
        }
    }
}
