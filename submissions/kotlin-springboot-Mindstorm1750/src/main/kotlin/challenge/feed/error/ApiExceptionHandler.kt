package challenge.feed.error

import kotlinx.serialization.json.Json
import org.slf4j.LoggerFactory
import org.springframework.http.HttpStatus
import org.springframework.http.MediaType
import org.springframework.http.ResponseEntity
import org.springframework.http.converter.HttpMessageNotReadableException
import org.springframework.web.ErrorResponse
import org.springframework.web.bind.annotation.ExceptionHandler
import org.springframework.web.bind.annotation.RestControllerAdvice

/**
 * Every error becomes `{"error":"<message>"}` with the status and message from SPEC.md.
 *
 * Bodies are serialized here, with the application's [Json], and written as text. Spring Framework 7.0.9
 * applies its kotlin-reflect-based `KotlinResponseBodyAdvice` to `@ExceptionHandler` results even when
 * kotlin-reflect is absent (controllers are guarded correctly), so error bodies avoid that converter path.
 */
@RestControllerAdvice
class ApiExceptionHandler(private val json: Json) {
    private val log = LoggerFactory.getLogger(javaClass)

    @ExceptionHandler(ApiException::class)
    fun handleApi(e: ApiException) = error(e.status, e.message)

    @ExceptionHandler(HttpMessageNotReadableException::class)
    fun handleUnreadable(e: HttpMessageNotReadableException) = error(HttpStatus.BAD_REQUEST, "malformed JSON body")

    @ExceptionHandler(Exception::class)
    fun handleOther(e: Exception): ResponseEntity<String> {
        // Spring MVC's own client errors (no handler -> 404 "not found", wrong method -> 405, ...).
        if (e is ErrorResponse) {
            val status = HttpStatus.valueOf(e.statusCode.value())
            return error(status, status.reasonPhrase.lowercase())
        }
        log.error("Unhandled exception", e)
        return error(HttpStatus.INTERNAL_SERVER_ERROR, "internal server error")
    }

    private fun error(status: HttpStatus, message: String): ResponseEntity<String> =
        ResponseEntity.status(status)
            .contentType(MediaType.APPLICATION_JSON)
            .body(json.encodeToString(ErrorBody.serializer(), ErrorBody(message)))
}
