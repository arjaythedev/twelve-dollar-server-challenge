package challenge.feed.config

import challenge.feed.TestDatabase
import challenge.feed.TestTokens
import org.springframework.beans.factory.annotation.Autowired
import org.springframework.boot.test.context.SpringBootTest
import org.springframework.boot.tomcat.TomcatWebServer
import org.springframework.boot.web.server.context.WebServerApplicationContext
import org.springframework.test.context.DynamicPropertyRegistry
import org.springframework.test.context.DynamicPropertySource
import kotlin.test.Test
import kotlin.test.assertEquals

@SpringBootTest(
    webEnvironment = SpringBootTest.WebEnvironment.RANDOM_PORT,
    properties = ["feed.jwt-secret=${TestTokens.SECRET}", "server.address=127.0.0.1"],
)
class TomcatConfigTest {
    @Autowired lateinit var context: WebServerApplicationContext

    @Test
    fun `idle keep-alive connections get small socket buffers`() {
        val connector = (context.webServer as TomcatWebServer).tomcat.connector
        assertEquals("2048", connector.getProperty("socket.appReadBufSize").toString())
        assertEquals("4096", connector.getProperty("socket.appWriteBufSize").toString())
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
