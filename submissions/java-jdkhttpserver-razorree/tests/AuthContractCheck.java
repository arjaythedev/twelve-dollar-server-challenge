package challenge;

import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

public final class AuthContractCheck {
    private static final String SECRET = "twelve-dollar-challenge";
    private static final Base64.Encoder B64 = Base64.getUrlEncoder().withoutPadding();

    private static String token(String username, long expiry) throws Exception {
        String payload = "{\"sub\":\"1\",\"username\":\"" + username + "\",\"exp\":" + expiry + "}";
        return signedPayload(payload);
    }

    private static String signedPayload(String payload) throws Exception {
        String header = B64.encodeToString("{\"alg\":\"HS256\"}".getBytes(StandardCharsets.UTF_8));
        String signed = header + "." + B64.encodeToString(payload.getBytes(StandardCharsets.UTF_8));
        Mac reference = Mac.getInstance("HmacSHA256");
        reference.init(new SecretKeySpec(SECRET.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        return signed + "." + B64.encodeToString(reference.doFinal(signed.getBytes(StandardCharsets.US_ASCII)));
    }

    private static String tampered(String token) {
        int split = token.lastIndexOf('.');
        byte[] signature = Base64.getUrlDecoder().decode(token.substring(split + 1));
        signature[0] ^= 1;
        return token.substring(0, split + 1) + B64.encodeToString(signature);
    }

    private static void reject(Auth auth, String token) throws Exception {
        try {
            auth.verify("Bearer " + token);
            throw new AssertionError("Invalid token was accepted");
        } catch (Server.ApiError expected) {
            if (expected.status != 401) throw new AssertionError("Unexpected authentication status");
        }
    }

    public static void main(String[] args) throws Exception {
        Auth auth = new Auth(SECRET);
        long now = Instant.now().getEpochSecond();
        String expired = token("expired", now - 1);
        for (String claim : List.of("", ",\"exp\":null", ",\"exp\":\"tomorrow\"", ",\"exp\":true")) {
            reject(auth, signedPayload("{\"sub\":\"1\",\"username\":\"test\"" + claim + "}"));
        }
        try (ExecutorService executor = Executors.newVirtualThreadPerTaskExecutor()) {
            List<Future<?>> tasks = new ArrayList<>();
            CyclicBarrier barrier = new CyclicBarrier(16);
            for (int worker = 0; worker < 16; worker++) {
                final int id = worker;
                tasks.add(executor.submit(() -> {
                    barrier.await();
                    for (int i = 0; i < 100; i++) {
                        String username = "worker-" + id + "-" + i + " café😀";
                        String signed = token(username, now + 3600);
                        Auth.User user = auth.verify("Bearer " + signed);
                        if (user.id() != 1 || !username.equals(user.username()))
                            throw new AssertionError("Authentication data leaked between requests");
                        reject(auth, tampered(signed));
                        reject(auth, expired);
                        if (!username.equals(auth.verify("Bearer " + signed).username()))
                            throw new AssertionError("MAC state was not reset");
                    }
                    return null;
                }));
            }
            for (Future<?> task : tasks) task.get(30, TimeUnit.SECONDS);
        }
        System.out.println("PASS HMAC concurrency, invalid signatures, expiry and repeated verification");
    }
}
