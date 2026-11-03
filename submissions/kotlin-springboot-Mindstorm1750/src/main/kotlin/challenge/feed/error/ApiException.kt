package challenge.feed.error

import kotlinx.serialization.Serializable
import org.springframework.http.HttpStatus

/**
 * An expected failure that maps straight to an error response. These are control flow, not bugs,
 * so they skip the (comparatively expensive) stack trace capture.
 */
sealed class ApiException(val status: HttpStatus, override val message: String) :
    RuntimeException(message, null, false, false)

class BadRequest(message: String) : ApiException(HttpStatus.BAD_REQUEST, message)

class Unauthorized(message: String) : ApiException(HttpStatus.UNAUTHORIZED, message)

class NotFound(message: String) : ApiException(HttpStatus.NOT_FOUND, message)

@Serializable
data class ErrorBody(val error: String)
