module app;

import asdf;
import jwtlited.phobos;

import vibe.core.core : runApplication;
import vibe.http.router : URLRouter;
import vibe.http.server :
    HTTPServerRequest,
    HTTPServerResponse,
    HTTPServerSettings,
    listenHTTP;

import d2sqlite3;
import std.datetime : Clock, SysTime;
import std.process : environment;
import std.string : strip, startsWith, endsWith, split;
import std.conv : to;
import std.typecons : Nullable;
import std.sumtype;
import std.algorithm : canFind;
import std.stdio : writeln;
import std.exception : enforce;
import std.array : appender;
import std.uni : byCodePoint;

struct Post
{
    long id;
    string body;
    string created_at;
    string author;
    long like_count;
}

struct PostEnvelope
{
    Post post;
}

struct FeedEnvelope
{
    Post[] posts;
}

struct HealthOK
{
    string status;
    string db;
    long uptime_s;
}

struct HealthDegraded
{
    string status;
    string db;
    string error;
}

struct LikeResponse
{
    bool liked;
    bool already_liked;
    long post_id;
}

struct CreatePostInput
{
    string body;
}

struct JWTHeader
{
    string alg;
}

struct JWTClaimsStringSub
{
    string sub;
    string username;
    double exp;
    Nullable!double nbf;
}

struct JWTClaimsNumberSub
{
    long sub;
    string username;
    double exp;
}

alias JWTClaims = SumType!(
    JWTClaimsStringSub,
    JWTClaimsNumberSub
);

struct AuthClaims
{
    long uid;
    string username;
}

bool validNbf(Nullable!double nbf)
{
    return nbf.isNull ||
        Clock.currTime.toUnixTime >= nbf.get;
}

string envOr(string key, string fallback)
{
    auto value = environment.get(key, "");
    return value.length ? value : fallback;
}

string json(T)(auto ref T value)
{
    return value.serializeToJson();
}

void respond(T)(
    scope HTTPServerResponse res,
    int status,
    auto ref T value
)
{
    res.statusCode = status;
    res.headers["Content-Type"] = "application/json; charset=utf-8";
    res.writeBody(json(value));
}

struct ErrorResponse
{
    string error;
}

void fail(
    scope HTTPServerResponse res,
    int status,
    string message
)
{
    ErrorResponse e;
    e.error = message;
    respond(res, status, e);
}

bool parsePostID(string value, out long id)
{
    try
    {
        id = to!long(value);
        return id > 0;
    }
    catch (Exception)
    {
        return false;
    }
}

/*
 * JWT verification is performed by jwtlited's HS256 handler.
 * ASDF is used for decoding the verified JWT header and claims.
 */
bool authenticate(
    ref HS256Handler jwt,
    string token,
    out AuthClaims auth
)
{
    if (!jwt.validate(token))
        return false;

    char[4096] headerBuffer;
    char[16384] payloadBuffer;

    if (!jwt.decode(token, headerBuffer[], payloadBuffer[]))
        return false;

    string headerJSON;
    string payloadJSON;

    /*
     * jwtlited writes decoded JSON into the provided buffers.
     * Find the JSON document terminator withoutValue treating the
     * entire fixed-size buffer as input.
     */
    auto headerEnd = headerBuffer[].length;
    auto payloadEnd = payloadBuffer[].length;

    while (headerEnd > 0 && headerBuffer[headerEnd - 1] == '\0')
        --headerEnd;
    while (payloadEnd > 0 && payloadBuffer[payloadEnd - 1] == '\0')
        --payloadEnd;

    headerJSON = headerBuffer[0 .. headerEnd].idup;
    payloadJSON = payloadBuffer[0 .. payloadEnd].idup;

    try
    {
        auto header = headerJSON.deserialize!JWTHeader;
        if (header.alg != "HS256")
            return false;
    }
    catch (Exception)
    {
        return false;
    }

    long uid;
    string username;
    double exp;
    bool nbfValid;

    JWTClaims claims = payloadJSON.deserialize!JWTClaims;

    claims.match!(
        (JWTClaimsStringSub c) {
            username = c.username;
            exp = c.exp;
            uid = to!long(c.sub);
            nbfValid = validNbf(c.nbf);
        },
        (JWTClaimsNumberSub c) {
            username = c.username;
            exp = c.exp;
            uid = c.sub;
            nbfValid = true;
        }
    );

    if (uid <= 0 || exp <= cast(double) Clock.currTime.toUnixTime)
        return false;

    if (!nbfValid)
        return false;

    auth.uid = uid;
    auth.username = username;
    return true;
}

struct FeedServer
{
    Database db;
    HS256Handler jwt;
    SysTime started;

    this(string dbPath, string secret)
    {
        db = Database(dbPath);

        db.run("PRAGMA journal_mode=WAL");
        db.run("PRAGMA synchronous=NORMAL");
        db.run("PRAGMA busy_timeout=5000");
        db.run("PRAGMA cache_size=-65536");
        db.run("PRAGMA temp_store=MEMORY");
        db.run("PRAGMA mmap_size=268435456");

        jwt.loadKey(secret);
        started = Clock.currTime;
    }

    Post readPost(Row row)
    {
        Post p;
        p.id = row.peek!long(0);
        p.body = row.peek!string(1);
        p.created_at = row.peek!string(2);
        p.author = row.peek!string(3);
        p.like_count = row.peek!long(4);
        return p;
    }

    string postQuery()
    {
        return `SELECT p.id, p.body, p.created_at, u.username,
            (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
            AS like_count
            FROM posts p JOIN users u ON u.id = p.user_id`;
    }

    bool findPost(long id, out Post p)
    {
        auto stmt = db.prepare(postQuery() ~ " WHERE p.id = ?");
        stmt.bind(1, id);

        auto rows = stmt.execute();
        foreach (row; rows)
        {
            p = readPost(row);
            return true;
        }
        return false;
    }

    void handle(
        scope HTTPServerRequest req,
        scope HTTPServerResponse res
    )
    {
        import vibe.stream.operations : readAllUTF8;

        auto path = req.requestURL.to!string;
        auto method = req.method.to!string;

        try
        {
            // GET /health
            if (method == "GET" && path == "/health")
            {
                try
                {
                    db.execute("SELECT 1").oneValue!long;

                    HealthOK outValue;
                    outValue.status = "ok";
                    outValue.db = "ok";
                    outValue.uptime_s =
                        (Clock.currTime - started).total!"seconds";

                    respond(res, 200, outValue);
                }
                catch (Exception e)
                {
                    HealthDegraded outValue;
                    outValue.status = "degraded";
                    outValue.db = "unreachable";
                    outValue.error = e.msg;
                    respond(res, 503, outValue);
                }
                return;
            }

            // GET /feed
            if (method == "GET" && path == "/feed")
            {
                auto stmt = db.prepare(
                    postQuery() ~
                    " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
                );

                FeedEnvelope outValue;
                auto rows = stmt.execute();

                foreach (row; rows)
                    outValue.posts ~= readPost(row);

                respond(res, 200, outValue);
                return;
            }

            // GET /posts/:id
            if (method == "GET" && path.startsWith("/posts/"))
            {
                auto suffix = path["/posts/".length .. $];

                if (suffix.canFind("/"))
                {
                    fail(res, 404, "not found");
                    return;
                }

                long id;
                if (!parsePostID(suffix, id))
                {
                    fail(res, 400, "invalid post id");
                    return;
                }

                Post p;
                if (!findPost(id, p))
                {
                    fail(res, 404, "post not found");
                    return;
                }

                PostEnvelope outValue;
                outValue.post = p;
                respond(res, 200, outValue);
                return;
            }

            bool createPost =
                method == "POST" && path == "/posts";

            bool likePost =
                method == "POST" &&
                path.startsWith("/posts/") &&
                path.endsWith("/like");

            if (!createPost && !likePost)
            {
                fail(res, 404, "not found");
                return;
            }

            // Protected routValuees require a Bearer JWT.
            auto authHeader = req.headers.get("Authorization", "");

            if (!authHeader.startsWith("Bearer "))
            {
                fail(res, 401, "missing bearer token");
                return;
            }

            AuthClaims auth;
            if (!authenticate(jwt, authHeader["Bearer ".length .. $], auth))
            {
                fail(res, 401, "invalid or expired token");
                return;
            }

            // POST /posts
            if (createPost)
            {
                string requestBody;

                try
                {
                    requestBody = req.bodyReader.readAllUTF8();
                }
                catch (Exception)
                {
                    fail(res, 400, "malformed JSON body");
                    return;
                }

                CreatePostInput input;
                try
                {
                    input = requestBody.deserialize!CreatePostInput;
                }
                catch (Exception)
                {
                    fail(res, 400, "malformed JSON body");
                    return;
                }

                auto body = strip(input.body).idup;
                if (body.length == 0)
                {
                    fail(res, 400, "body is required");
                    return;
                }

                size_t characterCount;
                foreach (dchar ch; body.byCodePoint)
                    ++characterCount;

                if (characterCount > 500)
                {
                    fail(res, 400,
                        "body must be at most 500 characters");
                    return;
                }

                auto stmt = db.prepare(
                    "INSERT INTO posts (user_id, body) "
                    ~ "VALUES (?, ?) RETURNING id, created_at"
                );

                stmt.bind(1, auth.uid);
                stmt.bind(2, body);

                long id;
                string createdAt;
                auto rows = stmt.execute();

                foreach (row; rows)
                {
                    id = row.peek!long(0);
                    createdAt = row.peek!string(1);
                    break;
                }

                Post p;
                p.id = id;
                p.body = body;
                p.created_at = createdAt;
                p.author = auth.username;
                p.like_count = 0;

                PostEnvelope outValue;
                outValue.post = p;
                respond(res, 201, outValue);
                return;
            }

            // POST /posts/:id/like
            auto suffix = path["/posts/".length .. $];
            suffix = suffix[0 .. $ - "/like".length];

            long id;
            if (!parsePostID(suffix, id))
            {
                fail(res, 400, "invalid post id");
                return;
            }

            auto stmt = db.prepare(
                "INSERT INTO likes (user_id, post_id) "
                ~ "SELECT ?, ? WHERE EXISTS "
                ~ "(SELECT 1 FROM posts WHERE id = ?) "
                ~ "ON CONFLICT (user_id, post_id) DO NOTHING"
            );

            stmt.bind(1, auth.uid);
            stmt.bind(2, id);
            stmt.bind(3, id);
            stmt.execute();

            auto changes =
                db.execute("SELECT changes()").oneValue!long;

            if (changes == 1)
            {
                LikeResponse outValue;
                outValue.liked = true;
                outValue.already_liked = false;
                outValue.post_id = id;
                respond(res, 201, outValue);
                return;
            }

            Post p;
            if (!findPost(id, p))
            {
                fail(res, 404, "post not found");
                return;
            }

            LikeResponse outValue;
            outValue.liked = true;
            outValue.already_liked = true;
            outValue.post_id = id;
            respond(res, 200, outValue);
        }
        catch (Exception e)
        {
            writeln("request error: ", e.msg);
            fail(res, 500, "internal server error");
        }
    }
}

void main()
{
    auto app = new FeedServer(
        envOr("SQLITE_PATH", "seed/feed.db"),
        envOr("JWT_SECRET", "twelve-dollar-challenge")
    );

    auto settings = new HTTPServerSettings;
    settings.port = to!ushort(envOr("PORT", "80"));
    settings.bindAddresses = [envOr("HOST", "0.0.0.0")];

    auto router = new URLRouter;
    router.get("/health", (scope HTTPServerRequest req,
                           scope HTTPServerResponse res)
    {
        app.handle(req, res);
    });

    router.get("/feed", (scope HTTPServerRequest req,
                         scope HTTPServerResponse res)
    {
        app.handle(req, res);
    });

    router.get("/posts/:id", (scope HTTPServerRequest req,
                              scope HTTPServerResponse res)
    {
        app.handle(req, res);
    });

    router.post("/posts", (scope HTTPServerRequest req,
                           scope HTTPServerResponse res)
    {
        app.handle(req, res);
    });

    router.post("/posts/:id/like", (scope HTTPServerRequest req,
                                    scope HTTPServerResponse res)
    {
        app.handle(req, res);
    });

    writeln("Listening on ",
        envOr("HOST", "0.0.0.0"), ":", settings.port);

    auto listener = listenHTTP(settings, router);
    scope (exit) listener.stopListening();

    runApplication();
}
