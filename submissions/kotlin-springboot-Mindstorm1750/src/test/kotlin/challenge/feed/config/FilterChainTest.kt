package challenge.feed.config

import challenge.feed.TestDatabase
import challenge.feed.TestTokens
import jakarta.servlet.ServletContext
import org.springframework.beans.factory.annotation.Autowired
import org.springframework.boot.test.context.SpringBootTest
import org.springframework.test.context.DynamicPropertyRegistry
import org.springframework.test.context.DynamicPropertySource
import kotlin.test.Test
import kotlin.test.assertEquals

@SpringBootTest(
    webEnvironment = SpringBootTest.WebEnvironment.RANDOM_PORT,
    properties = ["feed.jwt-secret=${TestTokens.SECRET}", "server.address=127.0.0.1"],
)
class FilterChainTest {
    @Autowired lateinit var servletContext: ServletContext

    @Test
    fun `every request passes through only the filters this API needs`() {
        // No form bodies (FormContentFilter), JSON is always UTF-8 (CharacterEncodingFilter), no WebSockets (WsFilter).
        assertEquals(setOf("requestContextFilter"), servletContext.filterRegistrations.keys)
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
