package challenge;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;
import org.springframework.web.servlet.NoHandlerFoundException;

@RestControllerAdvice
final class ErrorHandler {
    private static final Logger LOG = LoggerFactory.getLogger(ErrorHandler.class);

    @ExceptionHandler(ApiException.class)
    ResponseEntity<Responses.Error> api(ApiException exception) {
        return ResponseEntity.status(exception.status)
                .body(new Responses.Error(exception.getMessage()));
    }

    @ExceptionHandler(NoHandlerFoundException.class)
    ResponseEntity<Responses.Error> notFound() {
        return ResponseEntity.status(404).body(new Responses.Error("not found"));
    }

    @ExceptionHandler(Exception.class)
    ResponseEntity<Responses.Error> internal(Exception exception) {
        LOG.error("Request failed", exception);
        return ResponseEntity.status(500).body(new Responses.Error("internal server error"));
    }
}
