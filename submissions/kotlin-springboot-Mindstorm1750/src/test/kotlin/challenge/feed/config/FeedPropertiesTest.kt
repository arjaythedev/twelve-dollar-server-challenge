package challenge.feed.config

import challenge.feed.TestDatabase
import challenge.feed.TestTokens
import org.springframework.beans.factory.annotation.Autowired
import org.springframework.boot.test.context.SpringBootTest
import org.springframework.core.KotlinDetector
import org.springframework.test.context.DynamicPropertyRegistry
import org.springframework.test.context.DynamicPropertySource
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse

@SpringBootTest(webEnvironment = SpringBootTest.WebEnvironment.NONE, properties = ["feed.jwt-secret=${TestTokens.SECRET}"])
class FeedPropertiesTest {
    @Autowired lateinit var props: FeedProperties

    @Test
    fun `optional settings have defaults`() {
        assertEquals(4, props.readerPoolSize)
        assertEquals(536_870_912L, props.mmapSizeBytes)
    }

    @Test
    fun `kotlin-reflect is not on the classpath, so Spring invokes handlers with plain Method#invoke`() {
        assertFalse(KotlinDetector.isKotlinReflectPresent())
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
