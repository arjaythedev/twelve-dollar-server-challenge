package challenge;

import io.micronaut.http.HttpRequest;
import io.micronaut.http.HttpResponse;
import io.micronaut.json.JsonMapper;
import org.junit.jupiter.api.Test;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.concurrent.*;
import static org.junit.jupiter.api.Assertions.*;

class BehaviorTest {
    private final JsonMapper json = JsonMapper.createDefault();
    private String token(String algorithm, String payload) throws Exception {
        var encoder = Base64.getUrlEncoder().withoutPadding();
        String header = encoder.encodeToString(("{\"alg\":\"" + algorithm + "\"}").getBytes(StandardCharsets.UTF_8));
        String body = encoder.encodeToString(payload.getBytes(StandardCharsets.UTF_8));
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec("test-secret".getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        return "Bearer " + header + "." + body + "." + encoder.encodeToString(mac.doFinal((header + "." + body).getBytes(StandardCharsets.US_ASCII)));
    }
    @Test void authChecksSignatureAlgorithmExpiryAndPayload() throws Exception {
        var verifier = new JwtVerifier(json, "test-secret");
        String payload = "{\"sub\":\"1\",\"username\":\"alice\",\"exp\":4102444800}";
        assertEquals(new JwtVerifier.User(1, "alice"), verifier.verify(token("HS256", payload)));
        assertEquals("missing bearer token", assertThrows(ApiException.class, () -> verifier.verify(null)).getMessage());
        assertThrows(ApiException.class, () -> verifier.verify(token("HS512", payload)));
        assertThrows(ApiException.class, () -> verifier.verify(token("HS256", payload) + "x"));
        assertThrows(ApiException.class, () -> verifier.verify(token("HS256", payload.replace("4102444800", "1"))));
        assertThrows(ApiException.class, () -> verifier.verify(token("HS256", "{\"sub\":\"1\",\"username\":\"a\"}")));
        assertEquals("invalid token payload", assertThrows(ApiException.class,
            () -> verifier.verify(token("HS256", payload.replace("\"sub\":\"1\"", "\"sub\":1")))).getMessage());
    }
    @Test void authKeepsTokenShapeAndAsciiSubjectValidation() throws Exception {
        var verifier = new JwtVerifier(json, "test-secret");
        String payload = "{\"sub\":\"1\",\"username\":\"alice\",\"exp\":4102444800}";
        String valid = token("HS256", payload);
        for (String malformed : new String[]{"Bearer ", "Bearer .", "Bearer ..", valid + ".", valid + ".x",
                valid.substring(0, valid.lastIndexOf('.')), valid.substring(0, valid.lastIndexOf('.') + 1)})
            assertEquals("invalid or expired token", assertThrows(ApiException.class,
                () -> verifier.verify(malformed)).getMessage());
        for (String subject : new String[]{"", "0", "-1", "+1", "１", "1.5", "9223372036854775808"})
            assertEquals("invalid token payload", assertThrows(ApiException.class,
                () -> verifier.verify(token("HS256", payload.replace("\"sub\":\"1\"", "\"sub\":\"" + subject + "\"")))).getMessage());
        assertEquals(new JwtVerifier.User(1, "alice"),
            verifier.verify(token("HS256", payload.replace("\"sub\":\"1\"", "\"sub\":\"001\""))));
    }
    @Test void authenticationPrecedesBodyAndPathValidation() throws Exception {
        var verifier = new JwtVerifier(json, "test-secret");
        var controller = new FeedController(null, null, verifier, json);
        String valid = token("HS256", "{\"sub\":\"1\",\"username\":\"alice\",\"exp\":4102444800}");
        var request = HttpRequest.POST("/posts", "{");
        for (HttpResponse<?> response : new HttpResponse<?>[]{
                controller.create(request, "{").join(), controller.like("bad", request).join()}) {
            assertEquals(401, response.code());
            assertEquals(new Responses.Error("missing bearer token"), response.body());
        }
        request.getHeaders().set("Authorization", valid + "x");
        for (HttpResponse<?> response : new HttpResponse<?>[]{
                controller.create(request, "{").join(), controller.like("bad", request).join()}) {
            assertEquals(401, response.code());
            assertEquals(new Responses.Error("invalid or expired token"), response.body());
        }
        request.getHeaders().set("Authorization", valid);
        assertEquals(new Responses.Error("malformed JSON body"), controller.create(request, "{").join().body());
        assertEquals(new Responses.Error("invalid post id"), controller.like("bad", request).join().body());
    }
    @Test void authRemainsIndependentAcrossRequestsThreadsAndKeys() throws Exception {
        var verifier = new JwtVerifier(json, "test-secret");
        var otherKey = new JwtVerifier(json, "other-secret");
        String alice = token("HS256", "{\"sub\":\"1\",\"username\":\"alice\",\"exp\":4102444800}");
        String bob = token("HS256", "{\"sub\":\"2\",\"username\":\"bob\",\"exp\":4102444800}");
        String expired = token("HS256", "{\"sub\":\"1\",\"username\":\"alice\",\"exp\":1}");
        try (var workers = Executors.newFixedThreadPool(4)) {
            var tasks = new java.util.ArrayList<Future<?>>();
            for (int i = 0; i < 100; i++) tasks.add(workers.submit(() -> {
                assertEquals(new JwtVerifier.User(1, "alice"), verifier.verify(alice));
                assertThrows(ApiException.class, () -> verifier.verify(bob + "x"));
                assertThrows(ApiException.class, () -> otherKey.verify(alice));
                assertEquals("invalid or expired token", assertThrows(ApiException.class,
                    () -> verifier.verify(expired)).getMessage());
                assertEquals(new JwtVerifier.User(2, "bob"), verifier.verify(bob));
            }));
            for (var task : tasks) task.get(5, TimeUnit.SECONDS);
        }
    }
    @Test void validatesBodiesAndPreservesUnicode() {
        var controller = new FeedController(null, null, null, json);
        assertEquals("hello", controller.body("{\"body\":\"  hello  \"}"));
        String emoji = "😀".repeat(500);
        assertEquals(emoji, controller.body("{\"body\":\"" + emoji + "\"}"));
        assertEquals("x".repeat(500), controller.body("{\"body\":\"" + "x".repeat(500) + "\"}"));
        assertThrows(ApiException.class, () -> controller.body("{\"body\":\"" + emoji + "x\"}"));
        for (String raw : new String[]{"null", "[]", "{}", "{\"body\":true}", "{\"body\":\"  \"}"})
            assertEquals("body is required", assertThrows(ApiException.class, () -> controller.body(raw)).getMessage());
        assertEquals("malformed JSON body", assertThrows(ApiException.class, () -> controller.body("{")).getMessage());
        assertThrows(ApiException.class, () -> controller.body("{\"body\":\"" + "x".repeat(501) + "\"}"));
        assertEquals(1, FeedController.id("001"));
        assertEquals(Long.MAX_VALUE, FeedController.id("9223372036854775807"));
        for (String id : new String[]{"", "0", "-1", "+1", "１", "1\n", "abc", "1.5", "9223372036854775808"})
            assertThrows(ApiException.class, () -> FeedController.id(id));
    }
}
