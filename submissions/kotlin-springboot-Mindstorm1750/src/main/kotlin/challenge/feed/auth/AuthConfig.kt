package challenge.feed.auth

import challenge.feed.config.FeedProperties
import org.springframework.context.annotation.Bean
import org.springframework.context.annotation.Configuration
import java.time.Clock

@Configuration(proxyBeanMethods = false)
class AuthConfig {
    @Bean
    fun jwtVerifier(props: FeedProperties) = JwtVerifier(props.jwtSecret, Clock.systemUTC())
}
