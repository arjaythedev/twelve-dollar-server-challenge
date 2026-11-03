package feed.db;

/** A SQLite call failed. {@link #code()} is the primary result code, {@link #extendedCode()} the extended one. */
public final class DbException extends RuntimeException {

    private final int code;
    private final int extendedCode;

    DbException(String operation, int code, int extendedCode, String sqliteMessage) {
        super(operation + ": " + sqliteMessage + " (code " + code + ", extended " + extendedCode + ")");
        this.code = code;
        this.extendedCode = extendedCode;
    }

    public int code() {
        return code;
    }

    public int extendedCode() {
        return extendedCode;
    }
}
