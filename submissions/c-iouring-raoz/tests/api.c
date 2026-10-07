/* API semantics beyond test/test.sh: strict JSON, Unicode limits and trimming, JWT claims and headers,
 * foreign keys, concurrent likes and creates, and feed/post consistency under writes. Cases come from our
 * own checks and from other submissions' suites (PRs 3, 4, 14, 18 and 19 of the contest repository). */
#include "t.h"

static char *T; /* a valid token: user 1, username "integration ✓" */

/* POST /posts with a raw JSON body; returns the status */
static int create(const char *json, size_t n, const char *tok, resp *r) { return reqb("POST", "/posts", tok, json, n, r); }
static int create_s(const char *json, const char *tok, resp *r) { return create(json, strlen(json), tok, r); }

/* the "body" of a created post, decoded, compared by length (bodies may contain NUL) */
static int body_is(const resp *r, const char *want, size_t wn)
{
    const char *p = r->body ? strstr(r->body, "\"body\":\"") : NULL;
    if (!p) return 0;
    char *o = malloc(strlen(p) + 1);
    size_t n = jdecode(p + 8, o);
    int ok = n == wn && !memcmp(o, want, n);
    free(o);
    return ok;
}

static int is_error(const resp *r, int status, const char *msg)
{
    char *want = fmt("{\"error\":\"%s\"}", msg);
    int ok = r->status == status && streq(r->body, want);
    free(want);
    return ok;
}

static void strict_json(void)
{
    resp r = {0};
    static const struct { const char *s; size_t n; } bad[] = {
        {"{body:\"x\"}", 10}, {"{\"body\":\"x\",}", 13}, {"{\"body\":NaN}", 12}, {"{\"body\":/*x*/\"x\"}", 17},
        {"{'body':'x'}", 12}, {"{\"body\":\"x\"}\0junk", 17}, {"{\"body\":\"\xc0\x80\"}", 12},
        {"{\"body\":\"\xed\xa0\x80\"}", 13}, {"{\"body\":\"\xf4\x90\x80\x80\"}", 14},
        {"{\"body\":\"\xff\"}", 11}, {"{\"body\":\"\\ud800\"}", 17}, {"{\"body\":\"x\"}garbage", 19}, {"{\"body\":\"x\"} x", 14},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        if (create(bad[i].s, bad[i].n, T, &r) != 400 || !is_error(&r, 400, "malformed JSON body")) { ok = 0; printf("       body %zu: %d %s\n", i, r.status, r.body); }
    check(ok, "invalid JSON, overlong/surrogate/out-of-range UTF-8 and trailing bytes -> 400 malformed JSON body");

    static const char *ignored[] = {"{\"body\":\"valid\",\"ignored\":\"\\ud800\"}", "{\"body\":\"valid\",\"ignored\":{\"bad\":\"\\udfff\"}}",
                                    "{\"body\":\"valid\",\"ignored\":[\"\xff\"]}"};
    ok = 1;
    for (size_t i = 0; i < 3; i++) ok &= create_s(ignored[i], T, &r) == 400 && is_error(&r, 400, "malformed JSON body");
    check(ok, "invalid Unicode in ignored fields -> 400 malformed JSON body");

    static const char *nobody[] = {"null", "[]", "{}", "\"text\"", "123", "true", "[\"x\"]", "{\"body\":null}", "{\"body\":1}",
                                   "{\"body\":\"first\",\"body\":null}", "{\"body\":{\"nested\":\"text\"}}"};
    ok = 1;
    for (size_t i = 0; i < sizeof nobody / sizeof *nobody; i++)
        if (create_s(nobody[i], T, &r) != 400 || !is_error(&r, 400, "body is required")) { ok = 0; printf("       %s: %d %s\n", nobody[i], r.status, r.body); }
    check(ok, "missing or non-string body -> 400 body is required");

    static const struct { const char *raw, *want; } fields[] = {
        {"{\"body\":\"first\",\"body\":\"last\"}", "last"}, {"{\"b\\u006fdy\":\"escaped key\"}", "escaped key"},
        {"{\"extra\":{\"body\":\"ignored\",\"array\":[true,1,null,{}]},\"body\":\"kept\"}", "kept"},
        {"{\"body\":1,\"body\":\"ok dup\"}", "ok dup"}, {"{\"a\":{\"b\":[1,2,{\"c\":null}]},\"body\":\"nested ok\"}", "nested ok"},
    };
    ok = 1;
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
        ok &= create_s(fields[i].raw, T, &r) == 201 && body_is(&r, fields[i].want, strlen(fields[i].want));
    check(ok, "duplicate keys (last wins), escaped key names, nested values");

    ok = create_s("{\"body\":\"abc\\u0000def\"}", T, &r) == 201 && body_is(&r, "abc\0def", 7);
    check(ok, "escaped NUL inside the body is kept");
    free(r.body);
}

static void unicode(void)
{
    resp r = {0};
    static const unsigned cps[] = {0x1f600, 0x10000, 0x40000, 0x50000, 0x100000, 0x10ffff};
    int ok = 1;
    for (size_t i = 0; i < sizeof cps / sizeof *cps; i++) {
        char *c = cp(cps[i]), *v = repeat(c, 500), *v1 = repeat(c, 501);
        char *b = body_json(v, 1), *b1 = body_json(v1, 0);
        ok &= create_s(b, T, &r) == 201 && body_is(&r, v, strlen(v));       /* \u surrogate pairs */
        ok &= create_s(b1, T, &r) == 400 && is_error(&r, 400, "body must be at most 500 characters");
        free(c); free(v); free(v1); free(b); free(b1);
    }
    check(ok, "500 code points beyond the BMP fit (as surrogate escapes), 501 are rejected");

    char *e500 = repeat("\xc3\xa9", 500), *e501 = repeat("\xc3\xa9", 501);
    char *b500 = body_json(e500, 0), *b501 = body_json(e501, 0);
    ok = create_s(b500, T, &r) == 201 && create_s(b501, T, &r) == 400;
    check(ok, "500 two-byte characters fit, 501 are rejected");
    char *x6000 = repeat("x", 6000), *b6000 = body_json(x6000, 0);
    ok = create_s(b6000, T, &r) == 400 && is_error(&r, 400, "body must be at most 500 characters");
    check(ok, "6000-character body spanning several receive buffers -> 400");
    free(e500); free(e501); free(b500); free(b501); free(x6000); free(b6000);

    /* every Unicode White_Space character is trimmed; U+0085, U+180E and U+200B are not */
    static const unsigned ws[] = {9, 10, 11, 12, 13, 32, 0xa0, 0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006,
                                  0x2007, 0x2008, 0x2009, 0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000, 0xfeff};
    char spaces[256], *o = spaces;
    for (size_t i = 0; i < sizeof ws / sizeof *ws; i++) put_utf8(&o, ws[i]);
    *o = 0;
    char *t = fmt("%sedge%s", spaces, spaces), *b = body_json(t, 1), *bs = body_json(spaces, 0);
    ok = create_s(b, T, &r) == 201 && body_is(&r, "edge", 4);
    ok &= create_s(bs, T, &r) == 400 && is_error(&r, 400, "body is required");
    static const unsigned keep[] = {0x85, 0x180e, 0x200b};
    for (int i = 0; i < 3; i++) {
        char *c = cp(keep[i]), *v = fmt("%sedge%s", c, c), *bv = body_json(v, 1);
        ok &= create_s(bv, T, &r) == 201 && body_is(&r, v, strlen(v));
        free(c); free(v); free(bv);
    }
    check(ok, "Unicode whitespace trimmed at both ends; U+0085, U+180E, U+200B kept; whitespace only -> 400");
    free(t); free(b); free(bs);

    char *tq = jwt_user("7", "\"\xc3\xa9" "dith \\\"q\\\"\"");
    ok = create_s("{\"body\":\" \\u00e9\\ud83d\\ude00\\u3000 \"}", tq, &r) == 201 && body_is(&r, "\xc3\xa9\xf0\x9f\x98\x80", 6);
    char *author = jstr(r.body, "author");
    ok &= streq(author, "\xc3\xa9" "dith \"q\"");
    long long id = jnum(r.body, "id");
    char *path = fmt("/posts/%lld", id);
    ok &= req("GET", path, NULL, NULL, &r) == 200 && body_is(&r, "\xc3\xa9\xf0\x9f\x98\x80", 6);
    check(ok, "escaped Unicode decoded and trimmed, quoted username, read back");
    free(tq); free(author); free(path);

    /* bodies and usernames that need escaping both ways */
    const char *uname = "a\"b\\c caf\xc3\xa9 \xf0\x9f\x98\x80";
    char *ulit = jlit(uname, 0), *tu = jwt_user("1", ulit);
    static const struct { const char *in, *out; } esc[] = {
        {"\xef\xbb\xbf\xc2\xa0 caf\xc3\xa9 \xe2\x9c\x93 \xe2\x80\xa8", "caf\xc3\xa9 \xe2\x9c\x93"},
        {"quote \" slash \\ tab\t newline\n\x01" "end", "quote \" slash \\ tab\t newline\n\x01" "end"},
    };
    ok = 1;
    for (int i = 0; i < 2; i++) {
        char *bj = body_json(esc[i].in, 0);
        ok &= create_s(bj, tu, &r) == 201 && body_is(&r, esc[i].out, strlen(esc[i].out));
        char *a = jstr(r.body, "author");
        ok &= streq(a, uname);
        free(bj); free(a);
    }
    check(ok, "escaping round trip for bodies and the JWT username");
    free(ulit); free(tu);

    ok = 1;
    const char *names[] = {NULL, "second", "escaped \" name \\ \xf0\x9f\x98\x80", "third"};
    char *x1000 = repeat("x", 1000);
    names[0] = x1000;
    for (int i = 0; i < 4; i++) {
        char *l = jlit(names[i], 0), *tk = jwt_user("1", l);
        ok &= create_s("{\"body\":\"auth buffer reuse\"}", tk, &r) == 201;
        char *a = jstr(r.body, "author");
        ok &= streq(a, names[i]);
        free(l); free(tk); free(a);
    }
    check(ok, "usernames of 1000 characters and with escapes, back to back");
    free(x1000);
    free(r.body);
}

/* n requests on n connections, all sent before any response is read: concurrent for the server */
static void concurrent(int n, char **reqs, resp *out)
{
    conn *c = calloc(n, sizeof *c);
    for (int i = 0; i < n; i++) c[i] = copen();
    for (int i = 0; i < n; i++) csends(&c[i], reqs[i]);
    for (int i = 0; i < n; i++) { cread(&c[i], &out[i]); cclose(&c[i]); }
    free(c);
}

static void likes_and_feed(void)
{
    resp r = {0};
    char *emoji = repeat("\xf0\x9f\x98\x80", 500), *padded = fmt(" \n%s\t ", emoji), *b = body_json(padded, 0);
    int ok = create_s(b, T, &r) == 201 && body_is(&r, emoji, strlen(emoji));
    char *a = jstr(r.body, "author");
    long long id = jnum(r.body, "id");
    ok &= streq(a, "integration \xe2\x9c\x93") && id > 0 && jnum(r.body, "like_count") == 0;
    char *path = fmt("/posts/%lld", id), *lpath = fmt("/posts/%lld/like", id);
    ok &= req("GET", path, NULL, NULL, &r) == 200 && body_is(&r, emoji, strlen(emoji)) && jnum(r.body, "like_count") == 0;
    check(ok, "500 emoji with surrounding whitespace: created trimmed, author from the token, read back");
    free(a); free(padded); free(b);

    char *reqs[16];
    resp out[16] = {{0}};
    for (int i = 0; i < 16; i++) reqs[i] = rq("POST", lpath, T, NULL, 0);
    concurrent(16, reqs, out);
    int created = 0;
    ok = 1;
    for (int i = 0; i < 16; i++) {
        created += out[i].status == 201;
        char *want = fmt("{\"liked\":true,\"already_liked\":%s,\"post_id\":%lld}", out[i].status == 200 ? "true" : "false", id);
        ok &= (out[i].status == 200 || out[i].status == 201) && streq(out[i].body, want);
        free(want); free(out[i].body); free(reqs[i]);
    }
    ok &= created == 1;
    ok &= req("GET", path, NULL, NULL, &r) == 200 && jnum(r.body, "like_count") == 1;
    int infeed = 0;
    if (req("GET", "/feed", NULL, NULL, &r) == 200)
        for (int i = 0; i < feed_count(r.body); i++) {
            char *it = feed_item(r.body, i);
            if (jnum(it, "id") == id) infeed = jnum(it, "like_count") == 1;
            free(it);
        }
    check(ok && infeed, "16 concurrent identical likes: one 201, fifteen 200, like_count 1 in the post and the feed");

    /* a token that expires two seconds from now, with a fractional exp */
    double exp = now_real() + 2.25;
    char *p = fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%.3f}", exp), *shortlived = jwt(p);
    ok = req("POST", lpath, shortlived, NULL, &r) == 200;
    sleep_ms((int)((exp - now_real()) * 1000) + 100);
    ok &= req("POST", lpath, shortlived, NULL, &r) == 401 && is_error(&r, 401, "invalid or expired token");
    check(ok, "a token accepted before its fractional exp is rejected 100 ms after it");
    free(p); free(shortlived);

    /* unknown user: the schema's foreign keys reject it, and nothing else is affected */
    char *ghost = jwt_user("9223372036854775806", "\"ghost\"");
    ok = create_s("{\"body\":\"must not create orphan\"}", ghost, &r) == 500 && is_error(&r, 500, "internal server error");
    ok &= req("POST", lpath, ghost, NULL, &r) == 500 && is_error(&r, 500, "internal server error");
    ok &= req("GET", path, NULL, NULL, &r) == 200 && jnum(r.body, "like_count") == 1;
    ok &= req("GET", "/feed", NULL, NULL, &r) == 200 && feed_count(r.body) == 20 && !strstr(r.body, "must not create orphan");
    check(ok, "unknown user id -> 500 for create and like, later reads unaffected");
    free(ghost);

    /* 12 concurrent creates show up at the top of the feed in (created_at, id) order */
    char *creqs[12];
    resp cout[12] = {{0}};
    for (int i = 0; i < 12; i++) {
        char *bj = fmt("{\"body\":\"ordering %d\"}", i);
        creqs[i] = rq("POST", "/posts", T, bj, strlen(bj));
        free(bj);
    }
    concurrent(12, creqs, cout);
    ok = 1;
    long long ids[12];
    char *ts[12];
    for (int i = 0; i < 12; i++) {
        ok &= cout[i].status == 201;
        ids[i] = jnum(cout[i].body, "id");
        ts[i] = jstr(cout[i].body, "created_at");
    }
    for (int i = 0; i < 12; i++) /* newest first: created_at desc, id desc */
        for (int j = i + 1; j < 12; j++) {
            int c = ts[i] && ts[j] ? strcmp(ts[i], ts[j]) : 0;
            if (c < 0 || (c == 0 && ids[i] < ids[j])) {
                long long ti = ids[i]; ids[i] = ids[j]; ids[j] = ti;
                char *s = ts[i]; ts[i] = ts[j]; ts[j] = s;
            }
        }
    ok &= req("GET", "/feed", NULL, NULL, &r) == 200;
    for (int i = 0; i < 12 && ok; i++) {
        char *it = feed_item(r.body, i);
        ok &= jnum(it, "id") == ids[i];
        free(it);
    }
    check(ok, "12 concurrent creates appear at the top of the feed in created_at/id order");
    for (int i = 0; i < 12; i++) { free(creqs[i]); free(cout[i].body); free(ts[i]); }
    free(path); free(lpath); free(emoji);
    free(r.body);
}

static void tokens(void)
{
    resp r = {0};
    long e = (long)time(NULL) + 3600;
    /* (header, payload, expected message) */
    struct { const char *h; char *p; const char *msg; } bad[] = {
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\"}"), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":null}"), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":true}"), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":\"%ld\"}", e), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":\"3000000000\"}"), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":0}"), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":1e999}"), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld,\"nbf\":null}", e), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld,\"nbf\":true}", e), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld,\"nbf\":\"0\"}", e), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld,\"nbf\":\"bad\"}", e), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld,\"nbf\":%ld}", e, e), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":9999999999,\"nbf\":1e999}"), "invalid or expired token"},
        {HS256, fmt("{"), "invalid or expired token"},
        {HS256, fmt("{\"sub\":\"1\",\"username\":\"\xc3(\",\"exp\":%ld}", e), "invalid or expired token"},
        {"{\"alg\":\"HS512\",\"typ\":\"JWT\"}", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
        {"{\"alg\":\"hs256\"}", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
        {"{\"alg\":\"Hs256\"}", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
        {"{\"alg\":\"none\"}", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
        {"{\"alg\":0}", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
        {"{}", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
        {"null", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
        {"\"HS256\"", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
        {"{\"alg\":\"HS256\",\"x\":\"\xc3(\"}", fmt("{\"sub\":\"1\",\"username\":\"x\",\"exp\":%ld}", e), "invalid or expired token"},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char *t = jwt_raw(bad[i].h, bad[i].p);
        if (create_s("{\"body\":\"must not be inserted\"}", t, &r) != 401 || !is_error(&r, 401, bad[i].msg)) {
            ok = 0;
            printf("       %s . %s -> %d %s\n", bad[i].h, bad[i].p, r.status, r.body);
        }
        free(t); free(bad[i].p);
    }
    check(ok, "exp missing, not a finite number, or past; bad nbf; malformed JSON or UTF-8; alg not exactly HS256");

    static const char *subs[] = {"0", "1", "true", "\"0\"", "\"-1\"", "\"1.5\"", "\"abc\"", "null", "\"\"", "\"99999999999999999999\""};
    static const char *names[] = {"0", "true", "null", "{}", "[]"};
    ok = 1;
    for (size_t i = 0; i < sizeof subs / sizeof *subs; i++) {
        char *p = fmt("{\"sub\":%s,\"username\":\"x\",\"exp\":%ld}", subs[i], e), *t = jwt(p);
        ok &= create_s("{\"body\":\"x\"}", t, &r) == 401 && is_error(&r, 401, "invalid token payload");
        free(p); free(t);
    }
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char *p = fmt("{\"sub\":\"1\",\"username\":%s,\"exp\":%ld}", names[i], e), *t = jwt(p);
        ok &= create_s("{\"body\":\"x\"}", t, &r) == 401 && is_error(&r, 401, "invalid token payload");
        free(p); free(t);
    }
    check(ok, "sub not a positive integer string, or username not a string -> invalid token payload");

    char *p = fmt("{\"sub\":\"7\",\"username\":\"u7\",\"exp\":%ld}", e);
    char *t1 = jwt_raw("{\"typ\":\"JWT\",\"alg\":\"HS256\"}", p), *t2 = jwt_raw("{\"alg\":\"HS256\"}", p);
    char *p2 = fmt("{\"sub\":\"7\",\"username\":\"u7\",\"exp\":%.1f,\"nbf\":%ld}", (double)e + 0.5, (long)time(NULL) - 10), *t3 = jwt(p2);
    ok = create_s("{\"body\":\"hi\"}", t1, &r) == 201 && create_s("{\"body\":\"hi\"}", t2, &r) == 201 && create_s("{\"body\":\"hi\"}", t3, &r) == 201;
    check(ok, "header keys in another order, no typ, fractional exp and a past nbf are accepted");
    free(p); free(p2); free(t1); free(t2); free(t3);

    ok = req("POST", "/posts", "bad", "{bad", &r) == 401 && is_error(&r, 401, "invalid or expired token");
    ok &= req("POST", "/posts/abc/like", "bad", "{bad", &r) == 401;
    ok &= req("POST", "/posts/abc/like", NULL, NULL, &r) == 401 && is_error(&r, 401, "missing bearer token");
    ok &= req("POST", "/posts/abc/like", T, NULL, &r) == 400 && is_error(&r, 400, "invalid post id");
    conn c = copen();
    char *lower = fmt("POST /posts/1/like HTTP/1.1\r\nHost: x\r\nAuthorization: bearer %s\r\nContent-Length: 0\r\n\r\n", T);
    csends(&c, lower);
    ok &= cread(&c, &r) && is_error(&r, 401, "missing bearer token");
    cclose(&c);
    free(lower);
    check(ok, "auth is checked before the body and the id; a lowercase bearer scheme is missing");
    free(r.body);
}

static void routing(void)
{
    resp r = {0};
    int ok = req("POST", "/posts//like", NULL, NULL, &r) == 401 && req("POST", "/posts//like", T, NULL, &r) == 400 &&
             is_error(&r, 400, "invalid post id");
    check(ok, "POST /posts//like: 401 without a token, 400 invalid post id with one");
    /* Not followed from PR 19: there /posts/like is a like with a missing id. SPEC.md routes only
     * /posts/:id/like, so it is an unknown path. */
    ok = req("POST", "/posts/like", T, NULL, &r) == 404 && is_error(&r, 404, "not found");
    check(ok, "POST /posts/like is an unknown path -> 404");
    ok = req("GET", "/posts/000500000", NULL, NULL, &r) == 200 && jnum(r.body, "id") == 500000;
    ok &= req("GET", "/feed.json", NULL, NULL, &r) == 404;
    ok &= req("GET", "/posts/99999999999999999999999", NULL, NULL, &r) == 404;
    ok &= req("GET", "/feed?x=1", NULL, NULL, &r) == 200 && feed_count(r.body) == 20;
    check(ok, "zero-padded id, /feed.json 404, id beyond int64 404, query string ignored");
    free(r.body);
}

/* Random reads and writes on one keep-alive connection: every post read alone matches the same post in
 * the feed just before it. */
static void consistency(void)
{
    conn c = copen();
    resp r = {0}, one = {0};
    srand(7);
    int bad = 0, n = 0;
    for (int it = 0; it < 300; it++) {
        csends(&c, "GET /feed HTTP/1.1\r\nHost: x\r\n\r\n");
        if (!cread(&c, &r) || r.status != 200 || feed_count(r.body) != 20) { bad++; break; }
        for (int k = 0; k < 5; k++) {
            char *it_json = feed_item(r.body, rand() % 20);
            char *q = fmt("GET /posts/%lld HTTP/1.1\r\nHost: x\r\n\r\n", jnum(it_json, "id"));
            csends(&c, q);
            char *want = fmt("{\"post\":%s}", it_json);
            if (!cread(&c, &one) || !streq(one.body, want)) bad++;
            n++;
            free(it_json); free(q); free(want);
        }
        for (int k = rand() % 7; k > 0; k--) {
            char sub[16];
            snprintf(sub, sizeof sub, "%d", 1 + rand() % 20000);
            char *t = jwt_user(sub, "\"someone\"");
            char *it_json = feed_item(r.body, rand() % 20);
            char *q = fmt("POST /posts/%lld/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\n\r\n", jnum(it_json, "id"), t);
            csends(&c, q);
            if (!cread(&c, &one) || (one.status != 200 && one.status != 201)) bad++;
            free(t); free(it_json); free(q);
        }
        if (rand() % 10 < 3) {
            char *q = rq("POST", "/posts", T, "{\"body\":\"consistency\"}", 22);
            csends(&c, q);
            if (!cread(&c, &one) || one.status != 201) bad++;
            free(q);
        }
    }
    check(!bad, "300 rounds of feed, 5 post reads each, random likes and creates: %d reads, %d mismatches", n, bad);
    free(r.body); free(one.body);
    cclose(&c);
}

int main(void)
{
    srv_start();
    T = jwt_user("1", "\"integration \xe2\x9c\x93\"");
    strict_json();
    unicode();
    tokens();
    routing();
    likes_and_feed();
    consistency();
    printf("api: %d passed, %d failed\n", passes, fails);
    return fails != 0;
}
