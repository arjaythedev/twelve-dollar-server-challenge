package io.github.wolches.challenge12.rest;

import com.fasterxml.jackson.core.exc.StreamReadException;
import com.fasterxml.jackson.databind.exc.MismatchedInputException;
import io.github.wolches.challenge12.exception.*;
import io.github.wolches.challenge12.model.response.ErrorResponseDto;
import io.quarkus.security.AuthenticationFailedException;
import io.quarkus.security.UnauthorizedException;
import jakarta.enterprise.context.ApplicationScoped;
import jakarta.ws.rs.NotFoundException;
import jakarta.ws.rs.WebApplicationException;
import jakarta.ws.rs.core.Response;
import jakarta.ws.rs.core.UriInfo;
import org.jboss.resteasy.reactive.RestResponse;
import org.jboss.resteasy.reactive.server.ServerExceptionMapper;
import org.jboss.resteasy.reactive.server.UnwrapException;


@ApplicationScoped
@UnwrapException({WebApplicationException.class})
public class ErrorHandler {

    @ServerExceptionMapper
    public RestResponse<ErrorResponseDto> handle(NotFoundException e, UriInfo uriInfo) {
        return RestResponse.status(
                RestResponse.Status.NOT_FOUND,
                new ErrorResponseDto(UnknownPathException.MSG)
        );
    }

    @ServerExceptionMapper
    public Response handle(BusinessLogicException e, UriInfo uriInfo) {
        return Response
                .status(e.getStatus())
                .entity(new ErrorResponseDto(e.getMessage()))
                .build();
    }

    @ServerExceptionMapper
    public RestResponse<ErrorResponseDto> handle(UnauthorizedException e) {
        return RestResponse.status(
                RestResponse.Status.UNAUTHORIZED,
                new ErrorResponseDto(MissingBearerTokenException.MSG)
        );
    }

    @ServerExceptionMapper
    public RestResponse<ErrorResponseDto> handle(AuthenticationFailedException e) {
        return RestResponse.status(
                RestResponse.Status.UNAUTHORIZED,
                new ErrorResponseDto(InvalidOrExpiredTokenException.MSG)
        );
    }

    @ServerExceptionMapper
    public RestResponse<ErrorResponseDto> malformed(StreamReadException e) {
        return RestResponse.status(
                RestResponse.Status.BAD_REQUEST,
                new ErrorResponseDto(MalformedJsonBody.MSG)
        );
    }

    @ServerExceptionMapper
    public RestResponse<ErrorResponseDto> mismatched(MismatchedInputException e) {
        return RestResponse.status(
                RestResponse.Status.BAD_REQUEST,
                new ErrorResponseDto(MalformedJsonBody.MSG)
        );
    }

}