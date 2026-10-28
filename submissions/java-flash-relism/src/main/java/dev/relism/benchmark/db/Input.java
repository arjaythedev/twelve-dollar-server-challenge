package dev.relism.benchmark.db;

import dev.relism.fpr.core.ByteView;
import dev.relism.benchmark.auth.Tokens;
import dev.relism.benchmark.http.Json;
import dev.relism.flash.http.HttpException;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.SQLException;

/** JSON stays UTF-8: SQLite validates whole documents, extracts fields and trims post bodies. */
public final class Input {
    public record User(long id, byte[] name) {}
    private final Tokens tokens;
    private final PreparedStatement auth, body;

    public Input(Connection connection, Tokens tokens) throws SQLException {
        this.tokens = tokens;
        auth = connection.prepareStatement("""
                WITH input AS (SELECT CAST(?1 AS TEXT) h,CAST(?2 AS TEXT) p)
                SELECT CASE WHEN json_valid(h) AND json_valid(p) THEN
                  json_type(h)='object' AND json_type(p)='object'
                  AND json_type(h,'$.alg')='text' AND json_extract(h,'$.alg')='HS256'
                  AND json_type(p,'$.exp') IN ('integer','real')
                  AND (json_type(p,'$.nbf') IS NULL OR
                    (json_type(p,'$.nbf') IN ('integer','real') AND json_extract(p,'$.nbf')<=?3))
                  AND NOT EXISTS(SELECT 1 FROM json_each(h) GROUP BY key HAVING count(*)>1)
                  AND NOT EXISTS(SELECT 1 FROM json_each(p) GROUP BY key HAVING count(*)>1)
                  ELSE 0 END AS valid,
                  CASE WHEN json_valid(p) THEN json_extract(p,'$.exp') END AS expiry,
                  CASE WHEN json_valid(p) THEN CASE WHEN json_type(p,'$.sub')='text'
                    THEN CAST(json_extract(p,'$.sub') AS BLOB) END END AS subject,
                  CASE WHEN json_valid(p) THEN CASE WHEN json_type(p,'$.username')='text'
                    THEN CAST(json_extract(p,'$.username') AS BLOB) END END AS username
                FROM input
                """);
        body = connection.prepareStatement("""
                WITH input AS (SELECT CAST(?1 AS TEXT) j)
                SELECT CASE WHEN json_valid(j) THEN
                  NOT EXISTS(SELECT 1 FROM json_each(j) GROUP BY key HAVING count(*)>1)
                  ELSE 0 END AS valid,
                  CASE WHEN json_valid(j) THEN CASE WHEN json_type(j)='object'
                    AND json_type(j,'$.body')='text' THEN CAST(trim(json_extract(j,'$.body'),?2) AS BLOB)
                  END END AS body FROM input
                """);
        body.setString(2, "\t\n\u000b\f\r \u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005"
                + "\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff");
    }

    public User authenticate(ByteView authorization) throws SQLException {
        var proof = tokens.verify(authorization);
        auth.setBytes(1, proof.header());
        auth.setBytes(2, proof.payload());
        double now = System.currentTimeMillis() / 1000.0;
        auth.setDouble(3, now);
        try (var rows = auth.executeQuery()) {
            if (!rows.next() || rows.getInt(1) != 1) throw invalidToken();
            double expiry = rows.getDouble(2);
            if (!Double.isFinite(expiry) || expiry <= now) throw invalidToken();
            long id = Json.positiveId(rows.getBytes(3));
            byte[] name = rows.getBytes(4);
            if (id < 0 || name == null || Json.utf8Length(name) < 0)
                throw new HttpException(401, "invalid token payload");
            return new User(id, name);
        }
    }

    public byte[] postBody(byte[] bytes) throws SQLException {
        if (Json.utf8Length(bytes) < 0) throw new HttpException(400, "malformed JSON body");
        body.setBytes(1, bytes);
        try (var rows = body.executeQuery()) {
            if (!rows.next() || rows.getInt(1) != 1) throw new HttpException(400, "malformed JSON body");
            byte[] value = rows.getBytes(2);
            if (value == null || value.length == 0) throw new HttpException(400, "body is required");
            int length = Json.utf8Length(value);
            if (length < 0) throw new HttpException(400, "malformed JSON body");
            if (length > 500) throw new HttpException(400, "body must be at most 500 characters");
            return value;
        }
    }

    private static HttpException invalidToken() { return new HttpException(401, "invalid or expired token"); }
}
