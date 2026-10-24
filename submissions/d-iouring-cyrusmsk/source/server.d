module server;

import during;
import sqlite;
import jsonx;

import core.stdc.stdlib : calloc, free, getenv, malloc, realloc;
import core.stdc.string : memcmp, memcpy, memmove, memset, strlen;
import core.stdc.stdio : fprintf, stderr;
import core.sys.posix.arpa.inet : inet_addr;
import core.sys.posix.netinet.in_ : htonl, htons, sockaddr_in;
import core.sys.posix.signal : SIGINT, SIGTERM, signal;
import core.sys.posix.sys.resource : RLIMIT_NOFILE, getrlimit, rlimit, setrlimit;
import core.sys.posix.sys.socket : AF_INET, SOCK_STREAM;
import core.sys.posix.sys.socket : SO_REUSEADDR, SOL_SOCKET, bind, listen, setsockopt, sockaddr, socklen_t, socket;
import core.sys.posix.sys.time : time, time_t;
import core.sys.posix.unistd : close;
import core.sys.linux.errno : EAGAIN, EINTR, EINVAL, EOPNOTSUPP;


nothrow @nogc:

private enum int MAX_FD = 65536;
private enum int SOCK_NONBLOCK = 0x800;
private enum int SOCK_CLOEXEC = 0x80000;
private enum int IPPROTO_TCP = 6;
private enum int TCP_NODELAY = 1;
private enum size_t IN_CAP = 4096;
private enum size_t OUT_CAP = 32768;
private enum size_t FEED_CACHE_CAP = 32768;
private enum uint RING_ENTRIES = 32768;
private enum size_t PENDING_CAP = 2048;
private enum size_t HEADER_MAX = 3072;
private enum size_t BODY_MAX = 65536;
private enum ulong ACCEPT_TAG = ulong.max;

private enum int OP_RECV = 1;
private enum int OP_SEND = 2;

private enum string errorMissingToken = `{"error":"missing bearer token"}`;
private enum string errorInvalidPayload = `{"error":"invalid token payload"}`;
private enum string errorInvalidToken = `{"error":"invalid or expired token"}`;
private enum string errorNotFound = `{"error":"not found"}`;
private enum string errorInvalidPostId = `{"error":"invalid post id"}`;
private enum string errorPostNotFound = `{"error":"post not found"}`;
private enum string errorMalformedJson = `{"error":"malformed JSON body"}`;
private enum string errorBodyRequired = `{"error":"body is required"}`;
private enum string errorBodyTooLong = `{"error":"body must be at most 500 characters"}`;
private enum string errorInternal = `{"error":"internal server error"}`;

private enum string feedSql =
    `SELECT p.id, p.body, p.created_at, u.username,
     (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
     FROM posts p JOIN users u ON u.id = p.user_id
     ORDER BY p.created_at DESC, p.id DESC LIMIT 20`;

private enum string postSql =
    `SELECT p.id, p.body, p.created_at, u.username,
     (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
     FROM posts p JOIN users u ON u.id = p.user_id
     WHERE p.id = ?`;

private __gshared bool running = true;

private extern (C) void stopSignal(int)
{
    running = false;
}

private struct AuthClaims
{
    long uid;
    ubyte[128] username;
    size_t usernameLen;
}

private enum AuthStatus
{
    ok,
    missing,
    invalidToken,
    invalidPayload
}

private struct Conn
{
    int fd;
    bool open;
    bool sending;
    bool pendingSend;
    bool wantClose;
    ubyte* inbuf;
    size_t inLen;
    size_t inCap;
    ubyte* outbuf;
    size_t outLen;
    size_t outOff;
}

private struct Request
{
    const(ubyte)[] method;
    const(ubyte)[] path;
    const(ubyte)[] auth;
    size_t bodyStart;
    size_t totalLen;
    bool close;
}

private enum ParseRequestResult
{
    ok,
    incomplete,
    bad
}

private struct App
{
    Db db;
    sqlite3_stmt* healthStmt;
    sqlite3_stmt* feedStmt;
    sqlite3_stmt* postStmt;
    sqlite3_stmt* insertStmt;
    sqlite3_stmt* likeStmt;
    sqlite3_stmt* existsStmt;

    HmacSha256 hmac;
    time_t started;
    int listenFd;
    size_t openConns;
    size_t fdSoftLimit;
    size_t fdCloseAt;

    Uring ring;
    sockaddr_in acceptAddr;
    socklen_t acceptAddrLen;

    Conn*[MAX_FD] conns;
    Conn*[PENDING_CAP] pending;
    size_t pendingCount;

    bool inTransaction;
    bool acceptActive;
    int acceptMode;
    ubyte[FEED_CACHE_CAP] feedCache;
    size_t feedCacheLen;
    bool feedCacheValid;
}

private __gshared App app;

private ulong tagFor(int fd, int op)
{
    return (cast(ulong) cast(uint) fd << 8) | cast(ulong) cast(uint) op;
}

private int tagFd(ulong tag)
{
    return cast(int) (tag >> 8);
}

private int tagOp(ulong tag)
{
    return cast(int) (tag & 0xFF);
}

private const(char)[] envOr(const(char)* name, const(char)[] fallback)
{
    auto value = getenv(name);
    if (value is null || value[0] == '\0')
        return fallback;
    return cast(const(char)[]) value[0 .. strlen(value)];
}

private bool pathEq(const(ubyte)[] path, const(char)[] text)
{
    if (path.length != text.length)
        return false;
    foreach (i; 0 .. path.length)
    {
        if (path[i] != cast(ubyte) text[i])
            return false;
    }
    return true;
}

private bool startsWithPath(const(ubyte)[] path, const(char)[] prefix)
{
    if (path.length < prefix.length)
        return false;
    foreach (i; 0 .. prefix.length)
    {
        if (path[i] != cast(ubyte) prefix[i])
            return false;
    }
    return true;
}

private bool headerEq(const(ubyte)[] name, const(char)[] lower)
{
    if (name.length != lower.length)
        return false;
    foreach (i; 0 .. name.length)
    {
        ubyte c = name[i];
        if (c >= 'A' && c <= 'Z')
            c = cast(ubyte) (c + ('a' - 'A'));
        if (c != cast(ubyte) lower[i])
            return false;
    }
    return true;
}

private bool containsAsciiCI(const(ubyte)[] value, const(char)[] needle)
{
    if (value.length < needle.length)
        return false;

    foreach (i; 0 .. value.length - needle.length + 1)
    {
        bool match = true;
        foreach (j; 0 .. needle.length)
        {
            ubyte c = value[i + j];
            if (c >= 'A' && c <= 'Z')
                c = cast(ubyte) (c + ('a' - 'A'));
            if (c != cast(ubyte) needle[j])
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

private const(ubyte)[] trimBytes(const(ubyte)[] value)
{
    size_t start;
    size_t end = value.length;

    while (start < end && (value[start] == ' ' || value[start] == '\t'))
        ++start;
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n'))
        --end;

    return value[start .. end];
}

private size_t findCrlf(const(ubyte)[] data, size_t start, size_t end)
{
    for (size_t i = start; i + 1 < end; ++i)
    {
        if (data[i] == '\r' && data[i + 1] == '\n')
            return i;
    }
    return size_t.max;
}

private ParseRequestResult parseRequest(const(ubyte)[] data, out Request req)
{
    size_t lineEnd = findCrlf(data, 0, data.length);
    if (lineEnd == size_t.max)
        return data.length >= HEADER_MAX ? ParseRequestResult.bad : ParseRequestResult.incomplete;

    size_t firstSpace = size_t.max;
    size_t secondSpace = size_t.max;

    foreach (i; 0 .. lineEnd)
    {
        if (data[i] == ' ')
        {
            if (firstSpace == size_t.max)
                firstSpace = i;
            else
            {
                secondSpace = i;
                break;
            }
        }
    }

    if (firstSpace == size_t.max || secondSpace == size_t.max)
        return ParseRequestResult.bad;

    req.method = data[0 .. firstSpace];
    auto rawPath = data[firstSpace + 1 .. secondSpace];
    auto proto = data[secondSpace + 1 .. lineEnd];

    if (proto.length < 5 ||
        proto[0] != 'H' || proto[1] != 'T' || proto[2] != 'T' ||
        proto[3] != 'P' || proto[4] != '/')
        return ParseRequestResult.bad;

    bool http10 = proto.length >= 8 && proto[5] == '1' && proto[6] == '.' && proto[7] == '0';

    foreach (i; 0 .. rawPath.length)
    {
        if (rawPath[i] == '?')
        {
            rawPath = rawPath[0 .. i];
            break;
        }
    }

    req.path = rawPath;

    size_t pos = lineEnd + 2;
    size_t headerEnd = size_t.max;
    ulong contentLength;
    bool sawContentLength;
    bool closeConnection;
    bool keepAlive;

    while (pos < data.length)
    {
        size_t end = findCrlf(data, pos, data.length);
        if (end == size_t.max)
            return data.length >= HEADER_MAX ? ParseRequestResult.bad : ParseRequestResult.incomplete;

        if (end == pos)
        {
            headerEnd = end + 2;
            break;
        }

        size_t colon = size_t.max;
        foreach (i; pos .. end)
        {
            if (data[i] == ':')
            {
                colon = i;
                break;
            }
        }

        if (colon == size_t.max)
            return ParseRequestResult.bad;

        auto name = data[pos .. colon];
        auto value = trimBytes(data[colon + 1 .. end]);

        if (headerEq(name, "content-length"))
        {
            if (!value.length)
                return ParseRequestResult.bad;
            ulong parsed;
            foreach (ubyte c; value)
            {
                if (c < '0' || c > '9')
                    return ParseRequestResult.bad;
                parsed = parsed * 10 + (c - '0');
                if (parsed > BODY_MAX)
                    return ParseRequestResult.bad;
            }
            contentLength = parsed;
            sawContentLength = true;
        }
        else if (headerEq(name, "authorization"))
            req.auth = value;
        else if (headerEq(name, "connection"))
        {
            if (containsAsciiCI(value, "close"))
                closeConnection = true;
            if (containsAsciiCI(value, "keep-alive"))
                keepAlive = true;
        }

        pos = end + 2;
    }

    if (headerEnd == size_t.max)
        return data.length >= HEADER_MAX ? ParseRequestResult.bad : ParseRequestResult.incomplete;

    if (!sawContentLength)
        contentLength = 0;

    if (contentLength > BODY_MAX)
        return ParseRequestResult.bad;

    if (headerEnd + contentLength > data.length)
        return ParseRequestResult.incomplete;

    req.bodyStart = headerEnd;
    req.totalLen = headerEnd + cast(size_t) contentLength;
    req.close = closeConnection || (http10 && !keepAlive);
    return ParseRequestResult.ok;
}

private bool headerAlgIsHS256(const(ubyte)[] header)
{
    if (header.length < 2 || header[0] != '{' || header[header.length - 1] != '}')
        return false;

    size_t pos;
    size_t end = header.length;
    int depth;
    bool sawAlg;

    while (pos < end)
    {
        if (header[pos] != '"')
        {
            ++pos;
            continue;
        }

        size_t keyStart = ++pos;
        while (pos < end && header[pos] != '"')
        {
            if (header[pos] == '\\')
                return false;
            ++pos;
        }
        if (pos >= end)
            return false;
        auto key = header[keyStart .. pos];
        ++pos;

        while (pos < end && (header[pos] == ' ' || header[pos] == '\t'))
            ++pos;
        if (pos >= end || header[pos] != ':')
            continue;
        ++pos;
        while (pos < end && (header[pos] == ' ' || header[pos] == '\t'))
            ++pos;
        if (pos >= end || header[pos] != '"')
            continue;

        size_t valueStart = ++pos;
        while (pos < end && header[pos] != '"')
        {
            if (header[pos] == '\\')
                return false;
            ++pos;
        }
        if (pos >= end)
            return false;
        auto value = header[valueStart .. pos];
        ++pos;

        if (key.length == 3 && key[0] == 'a' && key[1] == 'l' && key[2] == 'g')
        {
            if (value.length != 5 || value[0] != 'H' || value[1] != 'S' ||
                value[2] != '2' || value[3] != '5' || value[4] != '6')
                return false;
            sawAlg = true;
        }
    }

    return sawAlg;
}

private AuthStatus authenticate(const(ubyte)[] header, time_t now, out AuthClaims claims)
{
    if (header.length < 7)
        return AuthStatus.missing;

    static immutable char[7] bearer = "Bearer ";
    foreach (i; 0 .. 7)
    {
        if (header[i] != cast(ubyte) bearer[i])
            return AuthStatus.missing;
    }

    auto token = header[7 .. $];
    if (!token.length)
        return AuthStatus.invalidToken;

    size_t dot1 = size_t.max;
    size_t dot2 = size_t.max;

    foreach (i; 0 .. token.length)
    {
        if (token[i] == '.')
        {
            if (dot1 == size_t.max)
                dot1 = i;
            else if (dot2 == size_t.max)
                dot2 = i;
            else
                return AuthStatus.invalidToken;
        }
    }

    if (dot1 == size_t.max || dot2 == size_t.max)
        return AuthStatus.invalidToken;

    ubyte[512] headerStorage;
    ubyte[4096] payloadStorage;
    ubyte[64] signatureStorage;
    size_t headerLen;
    size_t payloadLen;
    size_t signatureLen;

    if (!base64UrlDecode(cast(const(char)[]) token[0 .. dot1], headerStorage[], headerLen))
        return AuthStatus.invalidToken;
    if (!base64UrlDecode(cast(const(char)[]) token[dot1 + 1 .. dot2], payloadStorage[], payloadLen))
        return AuthStatus.invalidToken;
    if (!base64UrlDecode(cast(const(char)[]) token[dot2 + 1 .. $], signatureStorage[], signatureLen))
        return AuthStatus.invalidToken;

    if (signatureLen != 32 || headerLen == 0 || payloadLen == 0)
        return AuthStatus.invalidToken;

    if (!headerAlgIsHS256(headerStorage[0 .. headerLen]))
        return AuthStatus.invalidToken;

    ubyte[32] expected;
    app.hmac.sign(token[0 .. dot2], null, &expected);

    if (!constantTimeEqual(expected[], signatureStorage[0 .. 32]))
        return AuthStatus.invalidToken;

    ubyte[128] subStorage;
    ubyte[256] usernameStorage;
    Buf sub;
    Buf username;
    sub.init(subStorage[]);
    username.init(usernameStorage[]);

    double expires;
    auto result = parseJwtPayload(payloadStorage[0 .. payloadLen], sub, username, expires);

    if (result == JwtPayloadResult.malformed)
        return AuthStatus.invalidToken;
    if (result == JwtPayloadResult.badPayload)
        return AuthStatus.invalidPayload;
    if (result == JwtPayloadResult.badExpiry)
        return AuthStatus.invalidToken;
    if (expires <= cast(double) now)
        return AuthStatus.invalidToken;

    long uid;
    if (!parsePositiveId(cast(const(char)[]) sub.data[0 .. sub.len], uid))
        return AuthStatus.invalidPayload;
    if (username.len == 0 || username.len > claims.username.length)
        return AuthStatus.invalidPayload;

    claims.uid = uid;
    memcpy(claims.username.ptr, username.data, username.len);
    claims.usernameLen = username.len;
    return AuthStatus.ok;
}

private void closeConn(Conn* c)
{
    if (c is null)
        return;

    if (c.open)
    {
        close(c.fd);
        app.openConns--;
        app.conns[c.fd] = null;
        c.open = false;
    }

    if (c.inbuf !is null)
        free(c.inbuf);
    if (c.outbuf !is null)
        free(c.outbuf);

    free(c);
}

private void submitAccept()
{
    if (app.acceptActive)
        return;

    app.acceptAddrLen = sockaddr_in.sizeof;
    app.acceptActive = true;

    if (app.acceptMode == 2)
    {
        app.ring.putWith!((ref SubmissionEntry e, int fd, ref sockaddr_in addr, ref socklen_t len) {
            e.prepAccept(fd, addr, len, AcceptFlags.NONBLOCK | AcceptFlags.CLOEXEC);
            e.user_data = ACCEPT_TAG;
        })(app.listenFd, app.acceptAddr, app.acceptAddrLen);
        return;
    }

    app.acceptMode = 1;
    app.ring.putWith!((ref SubmissionEntry e, int fd, ref sockaddr_in addr, ref socklen_t len) {
        e.prepMultishotAccept(fd, addr, len, AcceptFlags.NONBLOCK | AcceptFlags.CLOEXEC);
        e.user_data = ACCEPT_TAG;
    })(app.listenFd, app.acceptAddr, app.acceptAddrLen);
}

private void submitRecv(Conn* c)
{
    if (!c.open || c.sending || c.pendingSend)
        return;

    if (c.inLen >= c.inCap)
    {
        closeConn(c);
        return;
    }

    app.ring.putWith!((ref SubmissionEntry e, int fd, ubyte[] buf, ulong ud) {
        e.prepRecv(fd, buf);
        e.user_data = ud;
    })(c.fd, c.inbuf[c.inLen .. c.inCap], tagFor(c.fd, OP_RECV));
}

private void submitSend(Conn* c)
{
    if (!c.open || c.outbuf is null || c.outOff >= c.outLen)
    {
        closeConn(c);
        return;
    }

    app.ring.putWith!((ref SubmissionEntry e, int fd, ubyte[] buf, ulong ud) {
        e.prepSend(fd, buf);
        e.user_data = ud;
    })(c.fd, c.outbuf[c.outOff .. c.outLen], tagFor(c.fd, OP_SEND));
}

private void flushPending()
{
    foreach (i; 0 .. app.pendingCount)
    {
        auto c = app.pending[i];
        if (c is null || !c.open)
            continue;
        c.pendingSend = false;
        c.sending = true;
        submitSend(c);
    }
    app.pendingCount = 0;
}

private void commitBatch()
{
    if (!app.inTransaction)
    {
        if (app.pendingCount)
            flushPending();
        return;
    }

    int rc = app.db.exec("COMMIT");
    app.inTransaction = false;

    if (rc != SQLITE_OK)
    {
        app.db.exec("ROLLBACK");
        foreach (i; 0 .. app.pendingCount)
        {
            auto c = app.pending[i];
            if (c is null || !c.open)
                continue;
            if (c.outbuf !is null)
            {
                free(c.outbuf);
                c.outbuf = null;
                c.outLen = 0;
                c.outOff = 0;
            }
            c.pendingSend = false;
            c.wantClose = true;
            prepareResponse(c, 500, cast(const(ubyte)[]) errorInternal, true);
            c.pendingSend = true;
        }
    }

    flushPending();
}

private void queueResponse(Conn* c)
{
    if (!app.inTransaction)
    {
        c.sending = true;
        submitSend(c);
        return;
    }

    if (app.pendingCount >= PENDING_CAP)
        commitBatch();

    if (app.inTransaction)
    {
        c.pendingSend = true;
        app.pending[app.pendingCount++] = c;
    }
    else
    {
        c.sending = true;
        submitSend(c);
    }
}

private const(char)[] statusText(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 404:
        return "Not Found";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "Error";
    }
}

private bool prepareResponse(Conn* c, int status, const(ubyte)[] body, bool closeAfter)
{
    ubyte[256] headerStorage;
    Buf header;
    header.init(headerStorage[]);

    header.put("HTTP/1.1 ");
    putLong(header, status);
    header.put(" ");
    header.put(statusText(status));
    header.put("\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: ");
    putLong(header, cast(long) body.length);
    header.put("\r\n");

    bool closeIt = closeAfter || c.wantClose || app.openConns >= app.fdCloseAt;
    if (closeIt)
    {
        c.wantClose = true;
        header.put("Connection: close\r\n");
    }
    else
        header.put("Connection: keep-alive\r\n");
    header.put("\r\n");

    if (header.overflow)
        return false;

    size_t total = header.len + body.length;
    auto mem = cast(ubyte*) malloc(total);
    if (mem is null)
        return false;

    memcpy(mem, header.data, header.len);
    if (body.length)
        memcpy(mem + header.len, body.ptr, body.length);

    if (c.outbuf !is null)
        free(c.outbuf);
    c.outbuf = mem;
    c.outLen = total;
    c.outOff = 0;
    return true;
}

private void sendJson(Conn* c, int status, const(char)[] body, bool closeAfter = false)
{
    if (!prepareResponse(c, status, cast(const(ubyte)[]) body, closeAfter))
    {
        c.wantClose = true;
        return;
    }
    queueResponse(c);
}

private void sendError(Conn* c, int status, const(char)[] body, bool closeAfter = false)
{
    sendJson(c, status, body, closeAfter);
}

private void writePostJson(ref Buf dest, sqlite3_stmt* stmt)
{
    dest.put(`{"id":`);
    putLong(dest, columnLong(stmt, 0));
    dest.put(`,"body":`);
    putJsonString(dest, columnText(stmt, 1));
    dest.put(`,"created_at":`);
    putJsonString(dest, columnText(stmt, 2));
    dest.put(`,"author":`);
    putJsonString(dest, columnText(stmt, 3));
    dest.put(`,"like_count":`);
    putLong(dest, columnLong(stmt, 4));
    dest.putByte('}');
}

private void handleHealth(Conn* c)
{
    int rc = sqlite3_step(app.healthStmt);
    bool ok = rc == SQLITE_ROW;
    sqlite3_reset(app.healthStmt);

    ubyte[128] storage;
    Buf dest;
    dest.init(storage[]);

    if (!ok)
    {
        dest.put(`{"status":"degraded","db":"unreachable","error":`);
        putJsonString(dest, "database error");
        dest.putByte('}');
        sendJson(c, 503, cast(const(char)[]) dest.data[0 .. dest.len], true);
        return;
    }

    dest.put(`{"status":"ok","db":"ok","uptime_s":`);
    putLong(dest, cast(long) (time(null) - app.started));
    dest.putByte('}');
    sendJson(c, 200, cast(const(char)[]) dest.data[0 .. dest.len]);
}

private void handleFeed(Conn* c)
{
    if (app.feedCacheValid)
    {
        sendJson(c, 200, cast(const(char)[]) app.feedCache[0 .. app.feedCacheLen]);
        return;
    }

    ubyte[32768] storage;
    Buf dest;
    dest.init(storage[]);
    dest.put(`{"posts":[`);

    int rc = sqlite3_step(app.feedStmt);
    bool first = true;
    while (rc == SQLITE_ROW)
    {
        if (!first)
            dest.putByte(',');
        first = false;
        writePostJson(dest, app.feedStmt);
        rc = sqlite3_step(app.feedStmt);
    }
    sqlite3_reset(app.feedStmt);

    if (rc != SQLITE_DONE || dest.overflow)
    {
        sendError(c, 500, errorInternal);
        return;
    }

    dest.put(`]}`);
    if (dest.len <= FEED_CACHE_CAP)
    {
        memcpy(app.feedCache.ptr, dest.data, dest.len);
        app.feedCacheLen = dest.len;
        app.feedCacheValid = true;
    }
    sendJson(c, 200, cast(const(char)[]) dest.data[0 .. dest.len]);
}

private void handleGetPost(Conn* c, const(ubyte)[] idText)
{
    long id;
    if (!parsePositiveId(cast(const(char)[]) idText, id))
    {
        sendError(c, 400, errorInvalidPostId);
        return;
    }

    bindLong(app.postStmt, 1, id);
    int rc = sqlite3_step(app.postStmt);

    if (rc == SQLITE_ROW)
    {
        ubyte[2048] storage;
        Buf dest;
        dest.init(storage[]);
        dest.put(`{"post":`);
        writePostJson(dest, app.postStmt);
        dest.putByte('}');
        sqlite3_reset(app.postStmt);

        if (dest.overflow)
        {
            sendError(c, 500, errorInternal);
            return;
        }
        sendJson(c, 200, cast(const(char)[]) dest.data[0 .. dest.len]);
        return;
    }

    sqlite3_reset(app.postStmt);
    if (rc == SQLITE_DONE)
        sendError(c, 404, errorPostNotFound);
    else
        sendError(c, 500, errorInternal);
}

private void handleCreate(Conn* c, const(ubyte)[] requestBody, ref AuthClaims auth)
{
    ubyte[4096] bodyStorage;
    Buf body;
    body.init(bodyStorage[]);

    auto result = parseCreateBody(requestBody, body);
    if (result == CreateBodyResult.malformed)
    {
        sendError(c, 400, errorMalformedJson);
        return;
    }
    if (result == CreateBodyResult.missing || result == CreateBodyResult.notString)
    {
        sendError(c, 400, errorBodyRequired);
        return;
    }

    auto trimmed = trimAscii(cast(const(char)[]) body.data[0 .. body.len]);
    if (!trimmed.length)
    {
        sendError(c, 400, errorBodyRequired);
        return;
    }
    if (codepointCount(trimmed) > 500)
    {
        sendError(c, 400, errorBodyTooLong);
        return;
    }

    if (!app.inTransaction)
    {
        if (app.db.exec("BEGIN IMMEDIATE") != SQLITE_OK)
        {
            sendError(c, 500, errorInternal);
            return;
        }
        app.inTransaction = true;
    }

    bindLong(app.insertStmt, 1, auth.uid);
    bindText(app.insertStmt, 2, trimmed);

    int rc = sqlite3_step(app.insertStmt);
    long id;
    ubyte[64] createdStorage;
    size_t createdLen;

    if (rc != SQLITE_ROW)
    {
        sqlite3_reset(app.insertStmt);
        sendError(c, 500, errorInternal);
        return;
    }

    id = columnLong(app.insertStmt, 0);
    auto createdAt = columnText(app.insertStmt, 1);
    if (createdAt.length >= createdStorage.length)
    {
        sqlite3_reset(app.insertStmt);
        sendError(c, 500, errorInternal);
        return;
    }
    memcpy(createdStorage.ptr, createdAt.ptr, createdAt.length);
    createdLen = createdAt.length;

    rc = sqlite3_step(app.insertStmt);
    sqlite3_reset(app.insertStmt);

    if (rc != SQLITE_DONE)
    {
        sendError(c, 500, errorInternal);
        return;
    }

    app.feedCacheValid = false;

    ubyte[2048] outStorage;
    Buf dest;
    dest.init(outStorage[]);
    dest.put(`{"post":{"id":`);
    putLong(dest, id);
    dest.put(`,"body":`);
    putJsonString(dest, trimmed);
    dest.put(`,"created_at":`);
    putJsonString(dest, cast(const(char)[]) createdStorage[0 .. createdLen]);
    dest.put(`,"author":`);
    putJsonString(dest, cast(const(char)[]) auth.username[0 .. auth.usernameLen]);
    dest.put(`,"like_count":0}}`);

    if (dest.overflow)
    {
        sendError(c, 500, errorInternal);
        return;
    }

    sendJson(c, 201, cast(const(char)[]) dest.data[0 .. dest.len]);
}

private void handleLike(Conn* c, const(ubyte)[] idText, ref AuthClaims auth)
{
    long id;
    if (!parsePositiveId(cast(const(char)[]) idText, id))
    {
        sendError(c, 400, errorInvalidPostId);
        return;
    }

    if (!app.inTransaction)
    {
        if (app.db.exec("BEGIN IMMEDIATE") != SQLITE_OK)
        {
            sendError(c, 500, errorInternal);
            return;
        }
        app.inTransaction = true;
    }

    bindLong(app.likeStmt, 1, auth.uid);
    bindLong(app.likeStmt, 2, id);
    bindLong(app.likeStmt, 3, id);
    int rc = sqlite3_step(app.likeStmt);
    sqlite3_reset(app.likeStmt);

    if (rc != SQLITE_DONE)
    {
        sendError(c, 500, errorInternal);
        return;
    }

    int changes = app.db.changes();

    if (changes == 1)
    {
        app.feedCacheValid = false;
        ubyte[128] storage;
        Buf dest;
        dest.init(storage[]);
        dest.put(`{"liked":true,"already_liked":false,"post_id":`);
        putLong(dest, id);
        dest.putByte('}');
        sendJson(c, 201, cast(const(char)[]) dest.data[0 .. dest.len]);
        return;
    }

    bindLong(app.existsStmt, 1, id);
    rc = sqlite3_step(app.existsStmt);
    bool exists = rc == SQLITE_ROW;
    sqlite3_reset(app.existsStmt);

    if (!exists)
    {
        sendError(c, 404, errorPostNotFound);
        return;
    }

    ubyte[128] storage;
    Buf dest;
    dest.init(storage[]);
    dest.put(`{"liked":true,"already_liked":true,"post_id":`);
    putLong(dest, id);
    dest.putByte('}');
    sendJson(c, 200, cast(const(char)[]) dest.data[0 .. dest.len]);
}

private void handleRequest(Conn* c, const(ubyte)[] data, ref Request req)
{
    auto body = data[req.bodyStart .. req.totalLen];
    bool isGet = req.method.length == 3 &&
                 req.method[0] == 'G' && req.method[1] == 'E' && req.method[2] == 'T';
    bool isPost = req.method.length == 4 &&
                  req.method[0] == 'P' && req.method[1] == 'O' &&
                  req.method[2] == 'S' && req.method[3] == 'T';

    if (isGet && pathEq(req.path, "/health"))
    {
        handleHealth(c);
        return;
    }

    if (isGet && pathEq(req.path, "/feed"))
    {
        handleFeed(c);
        return;
    }

    if (isGet && startsWithPath(req.path, "/posts/") && req.path.length > 7)
    {
        handleGetPost(c, req.path[7 .. $]);
        return;
    }

    if (isPost && pathEq(req.path, "/posts"))
    {
        AuthClaims auth;
        auto status = authenticate(req.auth, time(null), auth);
        if (status == AuthStatus.missing)
            sendError(c, 401, errorMissingToken);
        else if (status == AuthStatus.invalidPayload)
            sendError(c, 401, errorInvalidPayload);
        else if (status == AuthStatus.invalidToken)
            sendError(c, 401, errorInvalidToken);
        else
            handleCreate(c, body, auth);
        return;
    }

    if (isPost && startsWithPath(req.path, "/posts/") && req.path.length > 12 &&
        req.path[$ - 5 .. $] == cast(const(ubyte)[]) "/like")
    {
        AuthClaims auth;
        auto status = authenticate(req.auth, time(null), auth);
        if (status == AuthStatus.missing)
        {
            sendError(c, 401, errorMissingToken);
            return;
        }
        if (status == AuthStatus.invalidPayload)
        {
            sendError(c, 401, errorInvalidPayload);
            return;
        }
        if (status == AuthStatus.invalidToken)
        {
            sendError(c, 401, errorInvalidToken);
            return;
        }
        handleLike(c, req.path[7 .. $ - 5], auth);
        return;
    }

    sendError(c, 404, errorNotFound);
}

private void processInput(Conn* c)
{
    while (!c.sending && !c.pendingSend && c.inLen > 0)
    {
        Request req;
        auto result = parseRequest(c.inbuf[0 .. c.inLen], req);

        if (result == ParseRequestResult.incomplete)
        {
            submitRecv(c);
            return;
        }

        if (result == ParseRequestResult.bad)
        {
            sendError(c, 400, errorMalformedJson, true);
            return;
        }

        size_t consumed = req.totalLen;
        if (req.close)
            c.wantClose = true;
        handleRequest(c, c.inbuf[0 .. c.inLen], req);

        if (consumed < c.inLen)
            memmove(c.inbuf, c.inbuf + consumed, c.inLen - consumed);
        c.inLen -= consumed;

        if (c.wantClose || c.sending || c.pendingSend)
        {
            if (c.wantClose && !c.sending && !c.pendingSend)
                closeConn(c);
            return;
        }
    }

    if (!c.sending && !c.pendingSend)
        submitRecv(c);
}

private void handleAccept(int fd, CQEFlags flags)
{
    app.acceptActive = false;

    if (fd == -EINVAL || fd == -EOPNOTSUPP)
    {
        app.acceptMode = 2;
        submitAccept();
        return;
    }

    if (fd < 0)
    {
        submitAccept();
        return;
    }

    if (fd >= MAX_FD || app.conns[fd] !is null)
    {
        close(fd);
        if ((flags & CQEFlags.MORE) != 0)
            app.acceptActive = true;
        else
        {
            app.acceptMode = 2;
            submitAccept();
        }
        return;
    }

    auto c = cast(Conn*) calloc(1, Conn.sizeof);
    if (c is null)
    {
        close(fd);
        return;
    }

    c.inbuf = cast(ubyte*) malloc(IN_CAP);
    if (c.inbuf is null)
    {
        free(c);
        close(fd);
        return;
    }

    c.fd = fd;
    c.open = true;
    c.inCap = IN_CAP;
    app.conns[fd] = c;
    app.openConns++;
    submitRecv(c);

    if ((flags & CQEFlags.MORE) != 0)
        app.acceptActive = true;
    else
    {
        app.acceptMode = 2;
        submitAccept();
    }
}

private void handleRecv(Conn* c, int res)
{
    if (!c.open)
        return;

    if (res <= 0)
    {
        closeConn(c);
        return;
    }

    c.inLen += cast(size_t) res;
    processInput(c);
}

private void handleSend(Conn* c, int res)
{
    if (!c.open)
        return;

    if (res <= 0)
    {
        closeConn(c);
        return;
    }

    c.outOff += cast(size_t) res;

    if (c.outOff < c.outLen)
    {
        submitSend(c);
        return;
    }

    if (c.outbuf !is null)
    {
        free(c.outbuf);
        c.outbuf = null;
    }
    c.outLen = 0;
    c.outOff = 0;
    c.sending = false;

    if (c.wantClose)
    {
        closeConn(c);
        return;
    }

    if (c.inLen > 0)
        processInput(c);
    else
        submitRecv(c);
}

private void handleCqe()
{
    auto cqe = app.ring.front;
    ulong tag = cqe.user_data;

    if (tag == ACCEPT_TAG)
        handleAccept(cqe.res, cqe.flags);
    else
    {
        int fd = tagFd(tag);
        int op = tagOp(tag);
        if (fd >= 0 && fd < MAX_FD)
        {
            auto c = app.conns[fd];
            if (c !is null)
            {
                if (op == OP_RECV)
                    handleRecv(c, cqe.res);
                else if (op == OP_SEND)
                    handleSend(c, cqe.res);
            }
        }
    }

    app.ring.popFront();
}

private bool initFdLimit()
{
    rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return false;

    if (limit.rlim_cur < limit.rlim_max)
    {
        limit.rlim_cur = limit.rlim_max;
        setrlimit(RLIMIT_NOFILE, &limit);
        getrlimit(RLIMIT_NOFILE, &limit);
    }

    app.fdSoftLimit = cast(size_t) limit.rlim_cur;
    if (app.fdSoftLimit > 1024)
        app.fdCloseAt = app.fdSoftLimit - 256;
    else
        app.fdCloseAt = app.fdSoftLimit > 32 ? app.fdSoftLimit / 2 : 16;

    return true;
}

private bool initDatabase(const(char)* dbPath, const(char)* secret)
{
    return initDatabase(dbPath, secret, true);
}

private bool initDatabase(const(char)* dbPath, const(char)* secret, bool prepareStmts)
{
    if (sqlite3_initialize() != SQLITE_OK)
        return false;

    if (!app.db.open(dbPath))
        return false;

    if (app.db.exec("PRAGMA journal_mode=WAL".ptr) != SQLITE_OK)
        return false;
    if (app.db.exec("PRAGMA synchronous=NORMAL".ptr) != SQLITE_OK)
        return false;
    if (app.db.busyTimeout(5000) != SQLITE_OK)
        return false;
    if (app.db.exec("PRAGMA cache_size=-65536".ptr) != SQLITE_OK)
        return false;
    if (app.db.exec("PRAGMA temp_store=MEMORY".ptr) != SQLITE_OK)
        return false;
    if (app.db.exec("PRAGMA mmap_size=268435456".ptr) != SQLITE_OK)
        return false;
    if (app.db.exec("PRAGMA foreign_keys=OFF".ptr) != SQLITE_OK)
        return false;
    if (app.db.exec("PRAGMA wal_autocheckpoint=1000".ptr) != SQLITE_OK)
        return false;

    if (prepareStmts && !prepareStatements())
        return false;

    app.hmac.init(cast(const(char)[]) secret[0 .. strlen(secret)]);
    app.started = time(null);
    return true;
}


private bool prepareStatements()
{
    if (app.db.prepare("SELECT 1", &app.healthStmt) != SQLITE_OK)
        return false;

    if (app.db.prepare(feedSql, &app.feedStmt) != SQLITE_OK)
        return false;

    if (app.db.prepare(postSql, &app.postStmt) != SQLITE_OK)
        return false;

    if (app.db.prepare(
            "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at",
            &app.insertStmt) != SQLITE_OK)
        return false;

    if (app.db.prepare(
            "INSERT INTO likes (user_id, post_id) " ~
            "SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?) " ~
            "ON CONFLICT (user_id, post_id) DO NOTHING",
            &app.likeStmt) != SQLITE_OK)
        return false;

    if (app.db.prepare("SELECT 1 FROM posts WHERE id = ?", &app.existsStmt) != SQLITE_OK)
        return false;

    return true;
}

private bool initListener(const(char)* host, ushort port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, cast(socklen_t) int.sizeof);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, cast(socklen_t) int.sizeof);

    sockaddr_in addr;
    memset(&addr, 0, sockaddr_in.sizeof);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (bind(fd, cast(sockaddr*) &addr, cast(socklen_t) sockaddr_in.sizeof) != 0)
    {
        close(fd);
        return false;
    }

    if (listen(fd, 4096) != 0)
    {
        close(fd);
        return false;
    }

    app.listenFd = fd;
    return true;
}

private bool initRing()
{
    auto rc = app.ring.setup(RING_ENTRIES,
        SetupFlags.SINGLE_ISSUER | SetupFlags.DEFER_TASKRUN |
        SetupFlags.COOP_TASKRUN | SetupFlags.CLAMP);

    if (rc < 0)
        rc = app.ring.setup(RING_ENTRIES, SetupFlags.CLAMP);
    if (rc < 0)
        rc = app.ring.setup(1024);

    if (rc < 0)
        return false;

    app.ring.registerRingFd();
    return true;
}

private int runServer()
{
    submitAccept();

    while (running)
    {
        while (!app.ring.empty)
            handleCqe();

        commitBatch();

        int rc = app.ring.submitAndWait(1);
        if (rc < 0)
        {
            if (rc == -EINTR || rc == -EAGAIN)
                continue;
            return 1;
        }
    }

    return 0;
}


private enum string pgoSchema =
    "CREATE TABLE users (" ~
    "id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, " ~
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));" ~
    "CREATE TABLE posts (" ~
    "id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, body TEXT NOT NULL, " ~
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));" ~
    "CREATE TABLE likes (user_id INTEGER NOT NULL, post_id INTEGER NOT NULL, " ~
    "created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), " ~
    "PRIMARY KEY (user_id, post_id));" ~
    "CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);" ~
    "CREATE INDEX likes_post_id_idx ON likes (post_id);" ~
    "INSERT INTO users (id, username) VALUES (1, 'pgo_user');";

public int pgoTrain()
{
    if (!initDatabase(":memory:".ptr, "twelve-dollar-challenge".ptr, false))
    {
        fprintf(stderr, "pgo: initDatabase failed: %s\n", app.db.errmsg());
        return 1;
    }

    if (app.db.exec(pgoSchema.ptr) != SQLITE_OK)
    {
        fprintf(stderr, "pgo: schema failed: %s\n", app.db.errmsg());
        return 1;
    }

    if (!prepareStatements())
    {
        fprintf(stderr, "pgo: prepare failed: %s\n", app.db.errmsg());
        return 1;
    }

    ubyte[512] bodyStorage;
    foreach (i; 0 .. 128)
    {
        auto len = (i % 64) + 1;
        foreach (j; 0 .. len)
            bodyStorage[j] = cast(ubyte) ('a' + (j + i) % 26);
        bindLong(app.insertStmt, 1, 1);
        bindText(app.insertStmt, 2, cast(const(char)[]) bodyStorage[0 .. len]);
        if (sqlite3_step(app.insertStmt) != SQLITE_ROW)
        {
            fprintf(stderr, "pgo: insert failed: %s\n", app.db.errmsg());
            return 1;
        }
        sqlite3_step(app.insertStmt);
        sqlite3_reset(app.insertStmt);
    }

    ubyte[32768] feedStorage;
    Buf feed;
    feed.init(feedStorage[]);

    foreach (_; 0 .. 5000)
    {
        feed.len = 0;
        feed.overflow = false;
        feed.put(`{"posts":[`);
        int rc = sqlite3_step(app.feedStmt);
        bool first = true;
        while (rc == SQLITE_ROW)
        {
            if (!first)
                feed.putByte(',');
            first = false;
            writePostJson(feed, app.feedStmt);
            rc = sqlite3_step(app.feedStmt);
        }
        sqlite3_reset(app.feedStmt);

        bindLong(app.postStmt, 1, (cast(long) _ % 128) + 1);
        if (sqlite3_step(app.postStmt) == SQLITE_ROW)
        {
            ubyte[2048] postStorage;
            Buf post;
            post.init(postStorage[]);
            writePostJson(post, app.postStmt);
        }
        sqlite3_reset(app.postStmt);

        bindLong(app.likeStmt, 1, 1);
        bindLong(app.likeStmt, 2, (cast(long) _ % 128) + 1);
        bindLong(app.likeStmt, 3, (cast(long) _ % 128) + 1);
        sqlite3_step(app.likeStmt);
        sqlite3_reset(app.likeStmt);

        bindLong(app.existsStmt, 1, (cast(long) _ % 128) + 1);
        sqlite3_step(app.existsStmt);
        sqlite3_reset(app.existsStmt);
    }

    app.db.close();
    return 0;
}

public int run()
{
    auto hostText = envOr("HOST", "0.0.0.0");
    auto portText = envOr("PORT", "80");
    auto dbText = envOr("SQLITE_PATH", "/app/feed.db");
    auto secretText = envOr("JWT_SECRET", "twelve-dollar-challenge");

    ushort port;
    foreach (char ch; portText)
    {
        if (ch < '0' || ch > '9')
            return 2;
        port = cast(ushort) (port * 10 + (ch - '0'));
    }

    char[64] hostBuffer;
    if (hostText.length >= hostBuffer.length)
        return 2;
    memcpy(hostBuffer.ptr, hostText.ptr, hostText.length);
    hostBuffer[hostText.length] = '\0';

    char[512] dbBuffer;
    if (dbText.length >= dbBuffer.length)
        return 2;
    memcpy(dbBuffer.ptr, dbText.ptr, dbText.length);
    dbBuffer[dbText.length] = '\0';

    char[256] secretBuffer;
    if (secretText.length >= secretBuffer.length)
        return 2;
    memcpy(secretBuffer.ptr, secretText.ptr, secretText.length);
    secretBuffer[secretText.length] = '\0';

    initFdLimit();

    if (!initDatabase(dbBuffer.ptr, secretBuffer.ptr))
    {
        fprintf(stderr, "failed to initialize database: %s\n", app.db.errmsg());
        return 1;
    }

    if (!initListener(hostBuffer.ptr, port))
    {
        fprintf(stderr, "failed to bind %s:%u\n", hostBuffer.ptr, port);
        return 1;
    }

    if (!initRing())
    {
        fprintf(stderr, "failed to initialize io_uring\n");
        return 1;
    }

    signal(SIGTERM, &stopSignal);
    signal(SIGINT, &stopSignal);

    fprintf(stderr, "Listening on %s:%u\n", hostBuffer.ptr, port);

    auto rc = runServer();
    closeConn(null);
    if (app.listenFd)
        close(app.listenFd);
    app.db.close();
    return rc;
}
