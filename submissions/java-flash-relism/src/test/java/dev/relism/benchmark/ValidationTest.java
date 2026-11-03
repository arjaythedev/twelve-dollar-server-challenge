package dev.relism.benchmark;

import dev.relism.benchmark.auth.Tokens;
import dev.relism.benchmark.http.Json;
import dev.relism.benchmark.db.Input;
import java.sql.Connection;
import java.sql.DriverManager;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.AfterEach;
import dev.relism.flash.http.HttpException;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class ValidationTest {
    Connection connection;
    Input input;
    @BeforeEach void setup() throws Exception {
        connection = DriverManager.getConnection("jdbc:sqlite::memory:");
        input = new Input(connection, new Tokens("secret"));
    }
    @AfterEach void close() throws Exception { connection.close(); }

    @Test void bodyRejectsTrailingDocumentsAndInvalidUnicode() throws Exception {
        for (String input : new String[]{"{}", "null", "[]", "{\"body\":false}"}) {
            assertEquals("body is required", assertThrows(HttpException.class,
                    () -> this.input.postBody(input.getBytes(StandardCharsets.UTF_8))).getMessage());
        }
        for (String input : new String[]{"{\"body\":\"ok\"} {}", "{\"body\":\"ok\",\"body\":\"x\"}",
                "{\"body\":\"\\ud800\"}", "{\"body\":\"\\udc00\"}", "{\"body\":"}) {
            assertEquals("malformed JSON body", assertThrows(HttpException.class,
                    () -> this.input.postBody(input.getBytes(StandardCharsets.UTF_8))).getMessage());
        }
        assertArrayEquals("hello".getBytes(StandardCharsets.UTF_8), input.postBody("{\"body\":\"\\u00a0hello\\ufeff\"}".getBytes(StandardCharsets.UTF_8)));
        String emoji = "😀".repeat(500);
        assertArrayEquals(emoji.getBytes(StandardCharsets.UTF_8), input.postBody(("{\"body\":\"" + emoji + "\"}").getBytes(StandardCharsets.UTF_8)));
    }

    @Test void tokenVerifiesEachSignatureAndValidatesCompleteDocuments() throws Exception {
        var tokens = input;
        long expiry = System.currentTimeMillis() / 1000 + 3600;
        String claims = "{\"sub\":\"1\",\"username\":\"quote\\\"\\\\😀\",\"exp\":" + expiry + "}";
        var user = tokens.authenticate(header("Bearer " + sign("secret", "{\"alg\":\"HS256\"}", claims)));
        assertEquals(1, user.id());
        assertArrayEquals("quote\"\\😀".getBytes(StandardCharsets.UTF_8), user.name());
        assertEquals("invalid or expired token", rejected(tokens, sign("wrong", "{\"alg\":\"HS256\"}", claims)));
        assertEquals("invalid or expired token", rejected(tokens, sign("secret", "{\"alg\":\"none\"}", claims)));
        assertEquals("invalid or expired token", rejected(tokens, sign("secret", "{\"alg\":\"HS256\"}", claims + "{}")));
        assertEquals("invalid or expired token", rejected(tokens, sign("secret", "{\"alg\":\"HS256\"}",
                "{\"sub\":\"1\",\"username\":\"u\",\"exp\":1}")));
        assertEquals("invalid token payload", rejected(tokens, sign("secret", "{\"alg\":\"HS256\"}",
                "{\"sub\":1,\"username\":\"u\",\"exp\":" + expiry + "}")));
        assertEquals("invalid token payload", rejected(tokens, sign("secret", "{\"alg\":\"HS256\"}",
                "{\"sub\":\"9223372036854775808\",\"username\":\"u\",\"exp\":" + expiry + "}")));
    }

    @Test void idsNeverOverflow() {
        for (String input : new String[]{"0", "-1", "1.5", "1e3", "", "abc", "9223372036854775808"})
            assertEquals(-1, Json.positiveId(input));
        assertEquals(Long.MAX_VALUE, Json.positiveId(Long.toString(Long.MAX_VALUE)));
    }

    @Test void utf8RejectsInvalidScalarEncodings() {
        for (byte[] value : new byte[][]{
                {(byte) 0xc0, (byte) 0x80}, {(byte) 0xed, (byte) 0xa0, (byte) 0x80},
                {(byte) 0xf4, (byte) 0x90, (byte) 0x80, (byte) 0x80},
                {(byte) 0xe2, (byte) 0x82}, {(byte) 0x80}}) {
            assertEquals(-1, Json.utf8Length(value));
        }
        assertEquals(3, Json.utf8Length("aé😀".getBytes(StandardCharsets.UTF_8)));
    }

    private String rejected(Input tokens, String token) {
        return assertThrows(HttpException.class, () -> tokens.authenticate(header("Bearer " + token))).getMessage();
    }

    static dev.relism.fpr.core.ByteView header(String value) {
        if (value == null) return null;
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        return new dev.relism.fpr.core.ByteView() {
            public int length() { return bytes.length; }
            public byte byteAt(int index) { return bytes[index]; }
        };
    }

    static String sign(String key, String header, String claims) throws Exception {
        var b64 = Base64.getUrlEncoder().withoutPadding();
        String data = b64.encodeToString(header.getBytes(StandardCharsets.UTF_8)) + "."
                + b64.encodeToString(claims.getBytes(StandardCharsets.UTF_8));
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        return data + "." + b64.encodeToString(mac.doFinal(data.getBytes(StandardCharsets.US_ASCII)));
    }
}
