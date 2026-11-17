package challenge;

/** Thrown to short-circuit a request with a fixed error response. No stack trace: it is control flow. */
final class ApiError extends RuntimeException {

    final Resp resp;

    ApiError(Resp resp) {
        super(null, null, false, false);
        this.resp = resp;
    }
}
