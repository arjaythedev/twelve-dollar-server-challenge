package challenge.feed.config

import challenge.feed.auth.AuthUserArgumentResolver
import challenge.feed.auth.JwtVerifier
import org.springframework.context.annotation.Configuration
import org.springframework.web.method.support.HandlerMethodArgumentResolver
import org.springframework.web.servlet.config.annotation.WebMvcConfigurer

@Configuration(proxyBeanMethods = false)
class WebConfig(private val jwtVerifier: JwtVerifier) : WebMvcConfigurer {
    override fun addArgumentResolvers(resolvers: MutableList<HandlerMethodArgumentResolver>) {
        resolvers.add(AuthUserArgumentResolver(jwtVerifier))
    }
}
