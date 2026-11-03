package challenge.feed.config

import org.springframework.boot.tomcat.TomcatConnectorCustomizer
import org.springframework.context.annotation.Bean
import org.springframework.context.annotation.Configuration

/**
 * Serving directly means every user holds an idle keep-alive connection, and Tomcat keeps a read and a
 * write buffer on the heap for each one (8 KB each by default). Requests here are well under 2 KB and the
 * largest response (the feed) is ~7 KB, so smaller buffers cut the per-connection cost from ~16 KB to ~6 KB.
 */
@Configuration(proxyBeanMethods = false)
class TomcatConfig {
    @Bean
    fun smallSocketBuffers() = TomcatConnectorCustomizer { connector ->
        connector.setProperty("socket.appReadBufSize", "2048")
        connector.setProperty("socket.appWriteBufSize", "4096")
    }
}
