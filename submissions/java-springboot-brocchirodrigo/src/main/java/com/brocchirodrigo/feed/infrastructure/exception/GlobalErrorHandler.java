package com.brocchirodrigo.feed.infrastructure.exception;

import com.brocchirodrigo.feed.adapter.presenter.ErrorMessages;
import com.brocchirodrigo.feed.adapter.presenter.dto.ErrorResponse;
import org.springframework.core.Ordered;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.web.server.ServerWebExchange;
import org.springframework.web.server.WebExceptionHandler;
import reactor.core.publisher.Mono;
import tools.jackson.databind.json.JsonMapper;

public final class GlobalErrorHandler implements WebExceptionHandler, Ordered {

    private static final System.Logger LOGGER = System.getLogger(GlobalErrorHandler.class.getName());

    private final byte[] body;

    public GlobalErrorHandler(JsonMapper json) {
        this.body = json.writeValueAsBytes(new ErrorResponse(ErrorMessages.INTERNAL));
    }

    @Override
    public Mono<Void> handle(ServerWebExchange exchange, Throwable error) {
        var response = exchange.getResponse();
        if (response.isCommitted()) {
            return Mono.error(error);
        }
        LOGGER.log(System.Logger.Level.ERROR, "unhandled request error", error);
        response.setStatusCode(HttpStatus.INTERNAL_SERVER_ERROR);
        response.getHeaders().setContentType(MediaType.APPLICATION_JSON);
        return response.writeWith(Mono.just(response.bufferFactory().wrap(body)));
    }

    @Override
    public int getOrder() {
        return -2;
    }
}
