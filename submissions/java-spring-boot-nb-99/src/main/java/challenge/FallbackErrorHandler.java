package challenge;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.core.annotation.Order;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.server.reactive.ServerHttpResponse;
import org.springframework.stereotype.Component;
import org.springframework.web.server.ResponseStatusException;
import org.springframework.web.server.ServerWebExchange;
import org.springframework.web.server.WebExceptionHandler;
import reactor.core.publisher.Mono;
import tools.jackson.databind.json.JsonMapper;

/**
 * Writes the challenge's JSON errors for failures that never reach a controller or its advice,
 * instead of Spring Boot's default error body. It runs before Spring Boot's default handler, which
 * is ordered at -1.
 */
@Component
@Order(-2)
final class FallbackErrorHandler implements WebExceptionHandler {
    private static final Logger LOG = LoggerFactory.getLogger(FallbackErrorHandler.class);

    private final JsonMapper json;

    FallbackErrorHandler(JsonMapper json) {
        this.json = json;
    }

    @Override
    public Mono<Void> handle(ServerWebExchange exchange, Throwable error) {
        ServerHttpResponse response = exchange.getResponse();
        if (response.isCommitted()) {
            return Mono.error(error);
        }
        boolean notFound =
                error instanceof ResponseStatusException status
                        && status.getStatusCode().value() == 404;
        if (!(error instanceof ResponseStatusException)) {
            LOG.error("Request failed", error);
        }
        response.setStatusCode(notFound ? HttpStatus.NOT_FOUND : HttpStatus.INTERNAL_SERVER_ERROR);
        response.getHeaders().setContentType(MediaType.APPLICATION_JSON);
        byte[] body =
                json.writeValueAsBytes(
                        new Responses.Error(notFound ? "not found" : "internal server error"));
        return response.writeWith(Mono.just(response.bufferFactory().wrap(body)));
    }
}
