package server;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

public final class HttpHandler {
    private static final byte[] CRLF_CRLF = new byte[]{'\r', '\n', '\r', '\n'};
    private static final byte[] CT_JSON = "Content-Type: application/json\r\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] CONN_KEEP_ALIVE = "Connection: keep-alive\r\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] CONN_CLOSE = "Connection: close\r\n".getBytes(StandardCharsets.US_ASCII);

    private static final byte[] RES_200 = "HTTP/1.1 200 OK\r\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] RES_201 = "HTTP/1.1 201 Created\r\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] RES_400 = "HTTP/1.1 400 Bad Request\r\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] RES_401 = "HTTP/1.1 401 Unauthorized\r\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] RES_404 = "HTTP/1.1 404 Not Found\r\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] RES_500 = "HTTP/1.1 500 Internal Server Error\r\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] RES_503 = "HTTP/1.1 503 Service Unavailable\r\n".getBytes(StandardCharsets.US_ASCII);

    private static final String NOT_FOUND_JSON = "{\"error\":\"not found\"}";
    private static final String INVALID_POST_ID_JSON = "{\"error\":\"invalid post id\"}";

    private final long startTimeMs;

    public HttpHandler(long startTimeMs) {
        this.startTimeMs = startTimeMs;
    }

    public void handleConnection(Socket socket) {
        ConnectionTracker.onConnectionAccepted(socket);
        try {
            socket.setTcpNoDelay(true);
            InputStream in = socket.getInputStream();
            OutputStream out = socket.getOutputStream();

            byte[] buffer = new byte[4096];
            int bufferLen = 0;

            while (!socket.isClosed()) {
                ConnectionTracker.markIdle(socket);
                int read = in.read(buffer, bufferLen, buffer.length - bufferLen);
                if (read == -1) break; // Client closed connection
                ConnectionTracker.markActive(socket);
                bufferLen += read;

                while (true) {
                    int headerEnd = findHeaderEnd(buffer, bufferLen);
                    if (headerEnd == -1) {
                        // Incomplete headers, check if buffer is full
                        if (bufferLen == buffer.length) {
                            byte[] expanded = new byte[buffer.length * 2];
                            System.arraycopy(buffer, 0, expanded, 0, bufferLen);
                            buffer = expanded;
                        }
                        break; // Read more bytes from socket
                    }

                    // Headers complete, parse request line & headers
                    String headerText = new String(buffer, 0, headerEnd, StandardCharsets.US_ASCII);
                    int firstLineEnd = headerText.indexOf("\r\n");
                    if (firstLineEnd == -1) {
                        sendResponse(out, 400, "{\"error\":\"malformed request\"}", false);
                        return;
                    }

                    String requestLine = headerText.substring(0, firstLineEnd);
                    int sp1 = requestLine.indexOf(' ');
                    int sp2 = (sp1 == -1) ? -1 : requestLine.indexOf(' ', sp1 + 1);
                    if (sp1 == -1 || sp2 == -1) {
                        sendResponse(out, 400, "{\"error\":\"malformed request\"}", false);
                        return;
                    }

                    String method = requestLine.substring(0, sp1);
                    String path = requestLine.substring(sp1 + 1, sp2);

                    int contentLength = parseContentLength(headerText);
                    String authHeader = parseHeader(headerText, "authorization");
                    String connHeader = parseHeader(headerText, "connection");
                    boolean keepAlive = connHeader == null || !connHeader.equalsIgnoreCase("close");

                    int bodyStart = headerEnd + 4;
                    int totalRequestSize = bodyStart + contentLength;

                    if (bufferLen < totalRequestSize) {
                        // Body not fully received yet, ensure buffer has room
                        if (buffer.length < totalRequestSize) {
                            byte[] expanded = new byte[Math.max(buffer.length * 2, totalRequestSize)];
                            System.arraycopy(buffer, 0, expanded, 0, bufferLen);
                            buffer = expanded;
                        }
                        break; // Read more bytes
                    }

                    // Extract body if any
                    String body = (contentLength > 0)
                        ? new String(buffer, bodyStart, contentLength, StandardCharsets.UTF_8)
                        : null;

                    // Route and execute request
                    dispatch(out, method, path, authHeader, body, keepAlive);

                    // Shift remaining pipelined bytes if any
                    int remaining = bufferLen - totalRequestSize;
                    if (remaining > 0) {
                        System.arraycopy(buffer, totalRequestSize, buffer, 0, remaining);
                        bufferLen = remaining;
                    } else {
                        bufferLen = 0;
                        break; // Processed all available data in buffer
                    }

                    if (!keepAlive) {
                        return;
                    }
                }
            }
        } catch (Exception ignored) {
            // Socket disconnected / reset
        } finally {
            ConnectionTracker.onConnectionClosed(socket);
            try { socket.close(); } catch (Exception ignored) {}
        }
    }

    private void dispatch(OutputStream out, String method, String path, String authHeader, String body, boolean keepAlive) throws Exception {
        if ("GET".equals(method)) {
            if ("/health".equals(path)) {
                Database.DbResult res = Database.checkHealth(startTimeMs);
                sendResponse(out, res.status, res.body, keepAlive);
                return;
            }
            if ("/feed".equals(path)) {
                Database.DbResult res = Database.getFeed();
                sendResponse(out, res.status, res.body, keepAlive);
                return;
            }
            if (path.startsWith("/posts/")) {
                String idStr = path.substring(7);
                Long postId = parsePositiveId(idStr);
                if (postId == null) {
                    sendResponse(out, 400, INVALID_POST_ID_JSON, keepAlive);
                    return;
                }
                Database.DbResult res = Database.getPostById(postId);
                sendResponse(out, res.status, res.body, keepAlive);
                return;
            }
            sendResponse(out, 404, NOT_FOUND_JSON, keepAlive);
            return;
        }

        if ("POST".equals(method)) {
            if ("/posts".equals(path)) {
                JwtAuth.AuthResult auth = JwtAuth.authenticate(authHeader);
                if (auth.status != 200) {
                    sendResponse(out, auth.status, auth.errorJson, keepAlive);
                    return;
                }

                JsonUtil.PostBodyResult parsed = JsonUtil.parseCreatePostBody(body);
                if (parsed.status != 200) {
                    sendResponse(out, parsed.status, parsed.errorJson, keepAlive);
                    return;
                }

                Database.DbResult res = Database.createPost(auth.userId, parsed.body, auth.username);
                sendResponse(out, res.status, res.body, keepAlive);
                return;
            }

            if (path.startsWith("/posts/") && path.endsWith("/like")) {
                // IMPORTANT: Check authentication BEFORE checking post id (required by test suite)
                JwtAuth.AuthResult auth = JwtAuth.authenticate(authHeader);
                if (auth.status != 200) {
                    sendResponse(out, auth.status, auth.errorJson, keepAlive);
                    return;
                }

                String idStr = path.substring(7, path.length() - 5);
                Long postId = parsePositiveId(idStr);
                if (postId == null) {
                    sendResponse(out, 400, INVALID_POST_ID_JSON, keepAlive);
                    return;
                }

                Database.DbResult res = Database.likePost(auth.userId, postId);
                sendResponse(out, res.status, res.body, keepAlive);
                return;
            }

            sendResponse(out, 404, NOT_FOUND_JSON, keepAlive);
            return;
        }

        sendResponse(out, 404, NOT_FOUND_JSON, keepAlive);
    }

    private static Long parsePositiveId(String s) {
        if (s == null || s.isEmpty()) return null;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') return null;
        }
        try {
            long val = Long.parseLong(s);
            return (val > 0) ? val : null;
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static int findHeaderEnd(byte[] buf, int len) {
        for (int i = 0; i <= len - 4; i++) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                return i;
            }
        }
        return -1;
    }

    private static int parseContentLength(String headers) {
        int idx = headers.toLowerCase().indexOf("content-length:");
        if (idx == -1) return 0;
        int lineEnd = headers.indexOf("\r\n", idx);
        if (lineEnd == -1) lineEnd = headers.length();
        String val = headers.substring(idx + 15, lineEnd).trim();
        try {
            return Math.max(0, Integer.parseInt(val));
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    private static String parseHeader(String headers, String headerName) {
        int idx = headers.toLowerCase().indexOf(headerName.toLowerCase() + ":");
        if (idx == -1) return null;
        int lineEnd = headers.indexOf("\r\n", idx);
        if (lineEnd == -1) lineEnd = headers.length();
        return headers.substring(idx + headerName.length() + 1, lineEnd).trim();
    }

    private static void sendResponse(OutputStream out, int status, String jsonBody, boolean keepAlive) throws Exception {
        byte[] statusLine = switch (status) {
            case 200 -> RES_200;
            case 201 -> RES_201;
            case 400 -> RES_400;
            case 401 -> RES_401;
            case 404 -> RES_404;
            case 503 -> RES_503;
            default -> RES_500;
        };

        byte[] bodyBytes = jsonBody.getBytes(StandardCharsets.UTF_8);
        byte[] lenBytes = Integer.toString(bodyBytes.length).getBytes(StandardCharsets.US_ASCII);

        out.write(statusLine);
        out.write(CT_JSON);
        out.write("Content-Length: ".getBytes(StandardCharsets.US_ASCII));
        out.write(lenBytes);
        out.write("\r\n".getBytes(StandardCharsets.US_ASCII));
        out.write(keepAlive ? CONN_KEEP_ALIVE : CONN_CLOSE);
        out.write("\r\n".getBytes(StandardCharsets.US_ASCII));
        out.write(bodyBytes);
        out.flush();
    }
}

