// ReSharper disable CppUseInternalLinkage
#include <drogon/drogon.h>
#include <sqlite3.h>
#include <sys/resource.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sched.h>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "jwt.hpp"

using namespace drogon;

namespace ep {
    const auto kStart = std::chrono::steady_clock::now();
    static std::string g_secret;
    static std::string g_db_path;

    struct PostJson {
        int64_t id;
        std::string_view body, created_at, author;
        int64_t like_count;
    };


    struct NewPost {
        std::optional<std::string> body;
    };


    constexpr auto kPostSelect =
            "SELECT p.id, p.body, p.created_at, u.username,"
            " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"
            " FROM posts p JOIN users u ON u.id = p.user_id ";

    struct Db {
        sqlite3 *h = nullptr;
        sqlite3_stmt *feed = nullptr, *one = nullptr, *ins_post = nullptr, *ins_like = nullptr, *exists = nullptr,
                *health = nullptr;
        std::string error;

        sqlite3_stmt *prepare(const std::string &sql) {
            sqlite3_stmt *s = nullptr;
            if (sqlite3_prepare_v3(h, sql.c_str(), -1, SQLITE_PREPARE_PERSISTENT, &s, nullptr) != SQLITE_OK) {
                error = sqlite3_errmsg(h);
                return nullptr;
            }
            return s;
        }

        bool open() {
            if (sqlite3_open_v2(g_db_path.c_str(), &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr) !=
                SQLITE_OK) {
                error = sqlite3_errmsg(h);
                sqlite3_close(h);
                h = nullptr;
                return false;
            }
            sqlite3_exec(h,
                         "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA busy_timeout=5000;"
                         " PRAGMA mmap_size=1073741824; PRAGMA cache_size=-65536; PRAGMA temp_store=MEMORY;",
                         nullptr, nullptr, nullptr);
            const std::string select = kPostSelect;
            feed = prepare(select + "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
            one = prepare(select + "WHERE p.id = ?1");
            ins_post = prepare("INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at");
            ins_like = prepare(
                "INSERT INTO likes (user_id, post_id) SELECT ?1, id FROM posts WHERE id = ?2 ON CONFLICT DO NOTHING");
            exists = prepare("SELECT 1 FROM posts WHERE id = ?1");
            health = prepare("SELECT 1");
            return feed && one && ins_post && ins_like && exists && health;
        }
    };


    Db *db() {
        thread_local Db d;
        if (!d.feed || !d.one || !d.ins_post || !d.ins_like || !d.exists || !d.health) {
            sqlite3_close(d.h);
            d = Db{};
            if (!d.open()) return nullptr;
        }
        return &d;
    }

    struct Reset {
        sqlite3_stmt *s;
        ~Reset() { sqlite3_reset(s); }
    };


    static HttpResponsePtr reply(const HttpStatusCode code, std::string &&body) {
        auto r = HttpResponse::newHttpResponse();
        r->setStatusCode(code);
        r->setContentTypeCode(CT_APPLICATION_JSON);
        r->setBody(std::move(body));
        return r;
    }

    static HttpResponsePtr error(const HttpStatusCode code, const std::string_view msg) {
        std::string b = R"({"error":")";
        b += msg;
        b += "\"}";
        return reply(code, std::move(b));
    }

    static HttpResponsePtr internal_error() { return error(k500InternalServerError, "internal server error"); }

    static void append_int(std::string &out, const int64_t v) {
        char tmp[24];
        out.append(tmp, std::to_chars(tmp, tmp + sizeof tmp, v).ptr);
    }

    static std::string_view col_text(sqlite3_stmt *s, const int i) {
        return {
            reinterpret_cast<const char *>(sqlite3_column_text(s, i)), static_cast<size_t>(sqlite3_column_bytes(s, i))
        };
    }

    static void append_post(std::string &out, sqlite3_stmt *s) {
        thread_local std::string tmp;
        const PostJson p{
            sqlite3_column_int64(s, 0), col_text(s, 1), col_text(s, 2), col_text(s, 3), sqlite3_column_int64(s, 4)
        };
        (void) glz::write_json(p, tmp);
        out += tmp;
    }

    static std::optional<int64_t> parse_id(const std::string_view s) {
        int64_t id = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), id);
        if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size() || id <= 0) return std::nullopt;
        return id;
    }

    static HttpResponsePtr authenticate(const HttpRequestPtr &req, jwt::Identity &id) {
        switch (jwt::verify(req->getHeader("authorization"), g_secret, std::time(nullptr), id)) {
            case jwt::Status::Ok: return nullptr;
            case jwt::Status::MissingBearer: return error(k401Unauthorized, "missing bearer token");
            case jwt::Status::InvalidToken: return error(k401Unauthorized, "invalid or expired token");
            case jwt::Status::InvalidPayload: return error(k401Unauthorized, "invalid token payload");
        }
        return internal_error();
    }

    static HttpResponsePtr health() {
        const Db *d = db();
        if (d && sqlite3_step(d->health) == SQLITE_ROW) {
            Reset r{d->health};
            std::string b = R"({"status":"ok","db":"ok","uptime_s":)";
            append_int(
                b, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - kStart).count());
            b += '}';
            return reply(k200OK, std::move(b));
        }
        std::string msg = d ? sqlite3_errmsg(d->h) : "cannot open database";
        if (d) sqlite3_reset(d->health);
        std::string b = R"({"status":"degraded","db":"unreachable","error":)";
        std::string quoted;
        (void) glz::write_json(msg, quoted);
        b += quoted;
        b += '}';
        return reply(k503ServiceUnavailable, std::move(b));
    }

    static HttpResponsePtr feed() {
        const Db *d = db();
        if (!d) return internal_error();
        Reset r{d->feed};
        std::string b = R"({"posts":[)";
        b.reserve(8192);
        int rc;
        for (bool first = true; (rc = sqlite3_step(d->feed)) == SQLITE_ROW; first = false) {
            if (!first) b += ',';
            append_post(b, d->feed);
        }
        if (rc != SQLITE_DONE) return internal_error();
        b += "]}";
        return reply(k200OK, std::move(b));
    }

    static HttpResponsePtr get_post(const int64_t id) {
        const Db *d = db();
        if (!d) return internal_error();
        Reset r{d->one};
        sqlite3_bind_int64(d->one, 1, id);
        const int rc = sqlite3_step(d->one);
        if (rc == SQLITE_DONE) return error(k404NotFound, "post not found");
        if (rc != SQLITE_ROW) return internal_error();
        std::string b = R"({"post":)";
        append_post(b, d->one);
        b += '}';
        return reply(k200OK, std::move(b));
    }

    static HttpResponsePtr create_post(const HttpRequestPtr &req) {
        jwt::Identity who;
        if (auto e = authenticate(req, who)) return e;

        thread_local std::string raw;
        raw.assign(req->body());
        NewPost np;
        if (glz::validate_json(raw)) return error(k400BadRequest, "malformed JSON body");
        if (glz::read<jwt::kLoose>(np, raw) || !np.body) return error(k400BadRequest, "body is required");

        std::string_view body = *np.body;
        constexpr std::string_view kSpace = " \t\n\r\f\v";
        body.remove_prefix(std::min(body.find_first_not_of(kSpace), body.size()));
        if (const auto end = body.find_last_not_of(kSpace); end != std::string_view::npos)
            body.remove_suffix(
                body.size() - end - 1);
        if (body.empty()) return error(k400BadRequest, "body is required");
        if (std::ranges::count_if(body, [](const char c) {
            return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
        }) > 500)
            return error(k400BadRequest, "body must be at most 500 characters");

        const Db *d = db();
        if (!d) return internal_error();
        Reset r{d->ins_post};
        sqlite3_bind_int64(d->ins_post, 1, who.user_id);
        sqlite3_bind_text(d->ins_post, 2, body.data(), static_cast<int>(body.size()), SQLITE_STATIC);
        if (sqlite3_step(d->ins_post) != SQLITE_ROW) return internal_error();
        const int64_t id = sqlite3_column_int64(d->ins_post, 0);
        const std::string created_at(col_text(d->ins_post, 1));
        if (sqlite3_step(d->ins_post) != SQLITE_DONE) return internal_error();

        thread_local std::string tmp;
        (void) glz::write_json(PostJson{id, body, created_at, who.username, 0}, tmp);
        return reply(k201Created, R"({"post":)" + tmp + '}');
    }

    static HttpResponsePtr like_post(const HttpRequestPtr &req, const std::string_view id_str) {
        jwt::Identity who;
        if (auto e = authenticate(req, who)) return e;
        const auto id = parse_id(id_str);
        if (!id) return error(k400BadRequest, "invalid post id");

        const Db *d = db();
        if (!d) return internal_error();
        bool already = false;
        {
            Reset r{d->ins_like};
            sqlite3_bind_int64(d->ins_like, 1, who.user_id);
            sqlite3_bind_int64(d->ins_like, 2, *id);
            if (sqlite3_step(d->ins_like) != SQLITE_DONE) return internal_error();
        }
        if (sqlite3_changes(d->h) == 0) {
            Reset r{d->exists};
            sqlite3_bind_int64(d->exists, 1, *id);
            const int rc = sqlite3_step(d->exists);
            if (rc == SQLITE_DONE) return error(k404NotFound, "post not found");
            if (rc != SQLITE_ROW) return internal_error();
            already = true;
        }
        std::string b = R"({"liked":true,"already_liked":)";
        b += already ? "true" : "false";
        b += R"(,"post_id":)";
        append_int(b, *id);
        b += '}';
        return reply(already ? k200OK : k201Created, std::move(b));
    }

    static HttpResponsePtr route(const HttpRequestPtr &req) {
        const std::string &path = req->path();
        if (const auto method = req->method(); method == Get) {
            if (path == "/feed") return feed();
            if (path == "/health") return health();
            if (path.starts_with("/posts/")) {
                const std::string_view rest = std::string_view(path).substr(7);
                if (rest.find('/') == std::string_view::npos) {
                    const auto id = parse_id(rest);
                    return id ? get_post(*id) : error(k400BadRequest, "invalid post id");
                }
            }
        } else if (method == Post) {
            if (path == "/posts") return create_post(req);
            if (path.starts_with("/posts/") && path.ends_with("/like")) {
                const std::string_view mid = std::string_view(path).substr(7, path.size() - 12);
                if (mid.find('/') == std::string_view::npos) return like_post(req, mid);
            }
        }
        return error(k404NotFound, "not found");
    }
}

int main() {
    const char *path = std::getenv("SQLITE_PATH");
    const char *secret = std::getenv("JWT_SECRET");
    const char *host = std::getenv("HOST");
    const char *port = std::getenv("PORT");
    if (!path || !std::filesystem::exists(path)) {
        std::cerr << "SQLITE_PATH must name an existing database file (got " << (path ? path : "nothing") << ")\n";
        return 1;
    }
    ep::g_db_path = path;
    ep::g_secret = secret ? secret : "";

    rlimit lim{};
    getrlimit(RLIMIT_NOFILE, &lim);
    lim.rlim_cur = lim.rlim_max;
    setrlimit(RLIMIT_NOFILE, &lim);
    getrlimit(RLIMIT_NOFILE, &lim);
    const size_t max_conns = lim.rlim_cur > 512 ? lim.rlim_cur - 256 : 256;

    cpu_set_t cpu_set;
    const size_t cpus = sched_getaffinity(0, sizeof cpu_set, &cpu_set) == 0 ? std::max(1, CPU_COUNT(&cpu_set)) : 1;

    app()
            .setMaxConnectionNum(max_conns)
            .setLogLevel(trantor::Logger::kWarn)
            .setThreadNum(cpus)
            .setIdleConnectionTimeout(75)
            .enableGzip(false)
            .registerSyncAdvice(ep::route)
            .addListener(host ? host : "0.0.0.0", port ? std::atoi(port) : 80)
            .run();
}
