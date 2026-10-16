package feed;

/** An expected error response, {@code {"error":"<message>"}} with this status (see {@link ErrorMappers}). */
final class ApiError extends RuntimeException {
    final int status;

    ApiError(int status, String message) {
        super(message, null, false, false);
        this.status = status;
    }
}
