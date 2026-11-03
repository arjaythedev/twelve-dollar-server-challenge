package challenge;

final class ApiException extends RuntimeException {
    final int status;

    ApiException(int status, String message) {
        super(message);
        this.status = status;
    }
}
