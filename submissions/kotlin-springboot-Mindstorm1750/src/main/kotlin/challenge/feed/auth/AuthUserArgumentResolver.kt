package challenge.feed.auth

import challenge.feed.error.Unauthorized
import org.springframework.core.MethodParameter
import org.springframework.http.HttpHeaders
import org.springframework.web.bind.support.WebDataBinderFactory
import org.springframework.web.context.request.NativeWebRequest
import org.springframework.web.method.support.HandlerMethodArgumentResolver
import org.springframework.web.method.support.ModelAndViewContainer

/**
 * Supplies an [AuthUser] parameter from the `Authorization: Bearer <jwt>` header.
 * Spring resolves handler arguments in declaration order, so declare `AuthUser` first to check auth
 * before the request body is read.
 */
class AuthUserArgumentResolver(private val verifier: JwtVerifier) : HandlerMethodArgumentResolver {

    override fun supportsParameter(parameter: MethodParameter) = parameter.parameterType == AuthUser::class.java

    override fun resolveArgument(
        parameter: MethodParameter,
        mavContainer: ModelAndViewContainer?,
        webRequest: NativeWebRequest,
        binderFactory: WebDataBinderFactory?,
    ): AuthUser {
        val header = webRequest.getHeader(HttpHeaders.AUTHORIZATION)
        if (header == null || !header.startsWith(BEARER)) throw Unauthorized(JwtVerifier.MISSING_TOKEN)
        return verifier.verify(header.substring(BEARER.length))
    }

    private companion object {
        const val BEARER = "Bearer "
    }
}
