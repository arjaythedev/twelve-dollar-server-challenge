package challenge;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;
import org.springframework.web.server.ResponseStatusException;

@RestControllerAdvice
final class ErrorHandler {
    private static final Logger LOG = LoggerFactory.getLogger(ErrorHandler.class);

    @ExceptionHandler(ApiException.class)
    ResponseEntity<Responses.Error> api(ApiException exception) {
        return ResponseEntity.status(exception.status)
                .body(new Responses.Error(exception.getMessage()));
    }

    /** WebFlux also routes unmatched paths here as a 404 status exception. */
    @ExceptionHandler(ResponseStatusException.class)
    ResponseEntity<Responses.Error> status(ResponseStatusException exception) {
        boolean notFound = exception.getStatusCode().value() == 404;
        return ResponseEntity.status(notFound ? 404 : 500)
                .body(new Responses.Error(notFound ? "not found" : "internal server error"));
    }

    @ExceptionHandler(Exception.class)
    ResponseEntity<Responses.Error> internal(Exception exception) {
        LOG.error("Request failed", exception);
        return ResponseEntity.status(500).body(new Responses.Error("internal server error"));
    }
}
