package challenge;

import jakarta.servlet.RequestDispatcher;
import jakarta.servlet.http.HttpServletRequest;
import org.springframework.boot.webmvc.error.ErrorController;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
final class FallbackController implements ErrorController {
    @RequestMapping("/error")
    ResponseEntity<Responses.Error> error(HttpServletRequest request) {
        Object status = request.getAttribute(RequestDispatcher.ERROR_STATUS_CODE);
        boolean notFound = status == null || Integer.valueOf(404).equals(status);
        return ResponseEntity.status(notFound ? 404 : 500)
                .body(new Responses.Error(notFound ? "not found" : "internal server error"));
    }
}
