package feed;

import feed.Model.ErrorBody;
import io.quarkus.logging.Log;
import jakarta.ws.rs.NotFoundException;
import jakarta.ws.rs.WebApplicationException;
import java.util.Locale;
import org.jboss.resteasy.reactive.RestResponse;
import org.jboss.resteasy.reactive.RestResponse.Status;
import org.jboss.resteasy.reactive.server.ServerExceptionMapper;

/** Every error as SPEC.md's {@code {"error":"<message>"}}. */
final class ErrorMappers {

    @ServerExceptionMapper
    RestResponse<ErrorBody> apiError(ApiError e) {
        return RestResponse.status(Status.fromStatusCode(e.status), new ErrorBody(e.getMessage()));
    }

    @ServerExceptionMapper
    RestResponse<ErrorBody> notFound(NotFoundException e) {
        return RestResponse.status(Status.NOT_FOUND, new ErrorBody("not found"));
    }

    /** Quarkus REST's own errors (405, 415, ...), with their status. */
    @ServerExceptionMapper
    RestResponse<ErrorBody> webError(WebApplicationException e) {
        var status = e.getResponse().getStatusInfo();
        return RestResponse.status(Status.fromStatusCode(status.getStatusCode()),
                new ErrorBody(status.getReasonPhrase().toLowerCase(Locale.ROOT)));
    }

    @ServerExceptionMapper
    RestResponse<ErrorBody> unexpected(Exception e) {
        Log.error("request failed", e);
        return RestResponse.status(Status.INTERNAL_SERVER_ERROR, new ErrorBody("internal server error"));
    }
}
