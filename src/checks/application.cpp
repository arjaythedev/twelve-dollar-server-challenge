#include "app/application.hpp"
#include "http/session.hpp"
#include "json.hpp"

#include <openssl/evp.h>
#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{
constexpr std::string_view secret = "application-component-secret";

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct Close
{
    void operator()(sqlite3 *db) const noexcept
    {
        sqlite3_close(db);
    }
};

using Connection = std::unique_ptr<sqlite3, Close>;

Connection open(const std::string &path)
{
    sqlite3 *raw = nullptr;
    const int result =
        sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    Connection connection(raw);
    require(result == SQLITE_OK, "fixture open failed");

    return connection;
}

void execute(sqlite3 *db, const char *sql)
{
    require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "fixture SQL failed");
}

std::int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *statement = nullptr;
    require(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK,
            "fixture query prepare failed");
    const int stepped = sqlite3_step(statement);
    const auto value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    require(stepped == SQLITE_ROW, "fixture query returned no row");

    return value;
}

class Fixture
{
  public:
    Fixture()
    {
        char directory[] = "/tmp/td-application-XXXXXX";
        const char *created = ::mkdtemp(directory);
        require(created != nullptr, "fixture directory failed");
        directory_ = created;
        path = directory_ + "/feed.db";
        auto db = open(path);
        execute(db.get(), R"sql(
            CREATE TABLE users(id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
              created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
            CREATE TABLE posts(id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),
              body TEXT NOT NULL CHECK(length(body) BETWEEN 1 AND 500),
              created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')));
            CREATE TABLE likes(user_id INTEGER NOT NULL REFERENCES users(id),
              post_id INTEGER NOT NULL REFERENCES posts(id),
              created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
              PRIMARY KEY(user_id,post_id));
            CREATE INDEX posts_created_at_id_idx ON posts(created_at DESC,id DESC);
            CREATE INDEX posts_user_id_idx ON posts(user_id);
            CREATE INDEX likes_post_id_idx ON likes(post_id);
            INSERT INTO users(id,username) VALUES(1,'db-user'),(2,'second-user');
            INSERT INTO posts(id,user_id,body,created_at) VALUES
              (1,1,'old','2024-01-01T00:00:00.000Z'),
              (2,1,'tie-low','2025-01-01T00:00:00.000Z'),
              (3,1,'tie-high','2025-01-01T00:00:00.000Z');
        )sql");
    }

    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    std::string path;

  private:
    std::string directory_;
};

std::string base64url(std::string_view bytes)
{
    std::string output(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(output.data()),
                                        reinterpret_cast<const unsigned char *>(bytes.data()),
                                        static_cast<int>(bytes.size()));
    require(written >= 0, "fixture base64 encoding failed");
    output.resize(static_cast<std::size_t>(written));
    for (auto &character : output)
    {
        if (character == '+')
        {
            character = '-';
        }
        else if (character == '/')
        {
            character = '_';
        }
    }
    while (!output.empty() && output.back() == '=')
    {
        output.pop_back();
    }

    return output;
}

std::string authorization(std::string_view subject = "1", std::string_view username = "token-user",
                          bool expired = false)
{
    std::string claims = "{\"sub\":";
    tds::quote(claims, subject);
    claims += ",\"username\":";
    tds::quote(claims, username);
    claims += ",\"exp\":";
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    tds::integer(
        claims, expired ? 1 : std::chrono::duration_cast<std::chrono::seconds>(now).count() + 3600);
    claims += '}';
    std::string content = base64url("{\"alg\":\"HS256\"}") + '.' + base64url(claims);
    std::array<unsigned char, 32> signature{};
    std::size_t written = 0;
    require(EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, secret.data(), secret.size(),
                      reinterpret_cast<const unsigned char *>(content.data()), content.size(),
                      signature.data(), signature.size(), &written) &&
                written == signature.size(),
            "fixture signature failed");

    return "Bearer " + content + '.' +
           base64url({reinterpret_cast<const char *>(signature.data()), signature.size()});
}

tds::Document document(std::string_view input)
{
    auto parsed = tds::read_json(input);
    require(parsed.has_value(), "application returned invalid JSON");

    return std::move(*parsed);
}

std::string post_text(const tds::Response &response, std::string_view field)
{
    const auto parsed = document(response.body);
    auto *post = tds::member(yyjson_doc_get_root(parsed.get()), "post");
    auto *value = tds::member(post, field);
    require(yyjson_is_str(value), "post string field is missing");

    return std::string(tds::text_value(value));
}

std::int64_t post_number(const tds::Response &response, std::string_view field)
{
    const auto parsed = document(response.body);
    auto *post = tds::member(yyjson_doc_get_root(parsed.get()), "post");
    auto *value = tds::member(post, field);
    require(yyjson_is_int(value), "post numeric field is missing");

    return yyjson_get_sint(value);
}

tds::PreparedRequest prepare(tds::Application &app, tds::RequestHeadView head,
                             std::string_view body = {})
{
    auto accepted = app.accept_headers(head);
    require(accepted.has_value(), "valid fixture headers were rejected");
    auto prepared = app.prepare(*accepted, body);
    require(prepared.has_value(), "valid fixture request was rejected");

    return std::move(*prepared);
}

void expect_error(const std::expected<tds::PreparedRequest, tds::ApiError> &result,
                  tds::Status status, std::string_view message)
{
    require(!result && result.error().status == status && result.error().message == message,
            "request validation precedence or error changed");
}

void auth_and_validation_order()
{
    tds::Application app("/missing/application.db", std::string(secret));
    const auto missing = app.accept_headers({"POST", "/posts/abc/like", ""});
    require(!missing && missing.error().status == tds::Status::unauthorized &&
                missing.error().message == "missing bearer token",
            "invalid ID preceded missing auth");
    const auto expired = authorization("1", "token-user", true);
    const auto rejected = app.accept_headers({"POST", "/posts", expired});
    require(!rejected && rejected.error().message == "invalid or expired token",
            "expired token was accepted");

    const auto valid = authorization();
    auto like = app.accept_headers({"POST", "/posts/abc/like", valid});
    require(like && std::holds_alternative<tds::LikeHead>(*like),
            "like headers validated ID early");
    expect_error(app.prepare(*like, "{"), tds::Status::bad_request, "invalid post id");

    const auto oversized = authorization("9223372036854775808");
    auto create = app.accept_headers({"POST", "/posts", oversized});
    require(create && !std::get<tds::CreateHead>(*create).identity.user_id,
            "valid oversized subject was rejected at auth");
    expect_error(app.prepare(*create, "{"), tds::Status::bad_request, "malformed JSON body");
    expect_error(app.prepare(*create, "{\"body\":\" \"}"), tds::Status::bad_request,
                 "body is required");
    expect_error(app.prepare(*create, "{\"body\":\"valid\"}"), tds::Status::internal_error,
                 "internal server error");
    like = app.accept_headers({"POST", "/posts/abc/like", oversized});
    require(like.has_value(), "oversized subject changed like header acceptance");
    expect_error(app.prepare(*like, {}), tds::Status::bad_request, "invalid post id");
    like = app.accept_headers({"POST", "/posts/1/like", oversized});
    expect_error(app.prepare(*like, {}), tds::Status::internal_error, "internal server error");

    auto unknown = app.accept_headers({"POST", "/unknown", ""});
    require(unknown && std::holds_alternative<tds::MissingHead>(*unknown), "unknown route changed");
    expect_error(app.prepare(*unknown, {}), tds::Status::not_found, "not found");
}

void owned_create_and_unicode()
{
    tds::Application app("/missing/application.db", std::string(secret));
    auto token = authorization("1", "owned-token-user");
    auto head = app.accept_headers({"POST", "/posts", token});
    require(head.has_value(), "create header acceptance failed");
    token.assign(token.size(), 'x');
    std::string body = "{\"body\":\"old\",\"body\":\"last\"}";
    auto request = app.prepare(*head, body);
    require(request.has_value(), "duplicate-key create failed");
    body.assign(body.size(), 'x');
    const auto &command = std::get<tds::CreatePost>(std::get<tds::WriteCommand>(*request));
    require(command.body == "last" && command.username == "owned-token-user" &&
                command.user_id == tds::UserId{1},
            "prepared command borrowed JSON/token data or changed duplicate-key behavior");

    std::string unicode;
    for (int index = 0; index < 500; ++index)
    {
        unicode += "\xf0\x9f\x98\x80";
    }
    auto encoded = [&](std::string_view text) {
        std::string json = "{\"body\":";
        tds::quote(json, text);
        json += '}';

        return json;
    };
    request = app.prepare(*head, encoded("\xef\xbb\xbf\xc2\xa0" + unicode + "\xe3\x80\x80"));
    require(request &&
                std::get<tds::CreatePost>(std::get<tds::WriteCommand>(*request)).body == unicode,
            "Unicode trimming or character count changed");
    expect_error(app.prepare(*head, encoded(unicode + "x")), tds::Status::bad_request,
                 "body must be at most 500 characters");
}

void fresh_reads_and_write_results()
{
    Fixture fixture;
    tds::Application app(fixture.path, std::string(secret));
    auto reader = open(fixture.path);
    const auto feed = app.read(tds::ReadQuery{tds::Feed{}});
    const auto parsed = document(feed.body);
    auto *posts = tds::member(yyjson_doc_get_root(parsed.get()), "posts");
    require(feed.status == tds::Status::ok && yyjson_arr_size(posts) == 3 &&
                yyjson_get_sint(tds::member(yyjson_arr_get(posts, 0), "id")) == 3 &&
                yyjson_get_sint(tds::member(yyjson_arr_get(posts, 1), "id")) == 2 &&
                yyjson_get_sint(tds::member(yyjson_arr_get(posts, 2), "id")) == 1,
            "fresh feed changed timestamp/ID ordering");

    const auto token = authorization();
    auto prepared = prepare(app, {"POST", "/posts", token}, "{\"body\":\"  new \\\"post\\\"  \"}");
    std::array<tds::WriteCommand, 1> commands{std::get<tds::WriteCommand>(std::move(prepared))};
    std::vector<tds::WriteOutcome> outcomes(1);
    require(scalar(reader.get(), "SELECT count(*) FROM posts") == 3, "prepare performed a write");
    require(app.commit(commands, outcomes).has_value(), "create transaction failed");
    require(scalar(reader.get(), "SELECT count(*) FROM posts WHERE id=4") == 1,
            "create was not externally committed before reply encoding");
    {
        const std::array<tds::WriteCommand, 1> later_like{
            tds::LikePost{tds::UserId{2}, tds::PostId{4}}};
        std::array<tds::WriteOutcome, 1> later_outcome;
        require(app.commit(later_like, later_outcome).has_value() && later_outcome[0],
                "later batch could not like the committed create");
    }
    const auto created = app.write_reply(commands[0], outcomes[0]);
    require(created.status == tds::Status::created && post_number(created, "id") == 4 &&
                post_text(created, "body") == "new \"post\"" &&
                post_text(created, "author") == "token-user" &&
                post_number(created, "like_count") == 0,
            "created response lost token author, trimmed body or initial count");
    require(app.write_reply(commands[0], outcomes[0]).body == created.body &&
                scalar(reader.get(), "SELECT count(*) FROM posts") == 4,
            "reply encoding replayed a committed create");
    require(post_text(app.read(tds::ReadQuery{tds::GetPost{tds::PostId{4}}}), "author") ==
                "db-user",
            "post read reused the create token author");

    const auto second_token = authorization("2", "second-token");
    prepared = prepare(app, {"POST", "/posts/1/like", second_token});
    commands[0] = std::get<tds::WriteCommand>(std::move(prepared));
    require(app.commit(commands, outcomes).has_value(), "like transaction failed");
    const auto liked = app.write_reply(commands[0], outcomes[0]);
    require(liked.status == tds::Status::created &&
                liked.body == "{\"liked\":true,\"already_liked\":false,\"post_id\":1}" &&
                post_number(app.read(tds::ReadQuery{tds::GetPost{tds::PostId{1}}}), "like_count") ==
                    1,
            "new like response or fresh post count changed");
    require(app.commit(commands, outcomes).has_value(), "duplicate like transaction failed");
    const auto duplicate = app.write_reply(commands[0], outcomes[0]);
    require(duplicate.status == tds::Status::ok &&
                duplicate.body == "{\"liked\":true,\"already_liked\":true,\"post_id\":1}",
            "duplicate like response changed");

    commands[0] = tds::LikePost{tds::UserId{2}, tds::PostId{999}};
    require(app.commit(commands, outcomes).has_value(), "missing like prevented batch resolution");
    const auto missing_like = app.write_reply(commands[0], outcomes[0]);
    const auto missing_post = app.read(tds::ReadQuery{tds::GetPost{tds::PostId{999}}});
    require(missing_like.status == tds::Status::not_found &&
                missing_like.body == "{\"error\":\"post not found\"}" &&
                missing_post.status == tds::Status::not_found &&
                missing_post.body == missing_like.body,
            "missing post and missing like errors diverged");

    execute(reader.get(), "UPDATE users SET username='changed-db-user' WHERE id=1");
    require(post_text(app.read(tds::ReadQuery{tds::GetPost{tds::PostId{1}}}), "author") ==
                "changed-db-user",
            "post did not read external committed changes");
    require(app.read(tds::ReadQuery{tds::Feed{}}).body.find("changed-db-user") != std::string::npos,
            "feed retained a previous request's author data");
    const tds::WriteOutcome failed =
        std::unexpected(tds::WriteError{tds::WriteErrorCode::transaction, "fixture failure"});
    const auto error = app.write_reply(commands[0], failed);
    require(error.status == tds::Status::internal_error &&
                error.body == "{\"error\":\"internal server error\"}",
            "transaction error leaked backend details");
}

void fragmented_context_and_pipeline()
{
    tds::Application app("/missing/application.db", std::string(secret));
    tds::http::HttpSession session;
    const auto token = authorization();
    session.append("POST /posts/3/like HTTP/1.1\r\nHost: local\r\nAuthorization: " + token +
                   "\r\nContent-Length: 4\r\n\r\nab");
    require(session.consume().valid && session.headers_pending(),
            "fragment did not pause at headers");
    const auto &request = session.request();
    auto accepted = app.accept_headers({request.method, request.path, request.authorization});
    require(accepted && std::holds_alternative<tds::LikeHead>(*accepted),
            "fragmented like rejected");
    const auto id = std::get<tds::LikeHead>(*accepted).id;
    require(id.data() == session.request().path.data() + 7, "header context copied the path ID");
    session.accept_headers();
    require(session.consume().valid && !session.complete(), "partial body completed too early");
    session.append("cdGET /feed HTTP/1.1\r\nHost: local\r\n\r\n");
    require(session.consume().valid && session.complete() &&
                std::get<tds::LikeHead>(*accepted).id.data() == id.data(),
            "body fragment invalidated accepted path data");
    const auto prepared = app.prepare(*accepted, session.request().body);
    require(prepared && std::get<tds::LikePost>(std::get<tds::WriteCommand>(*prepared)).post_id ==
                            tds::PostId{3},
            "fragmented message did not produce its owned command");

    session.reset();
    require(session.consume().valid && session.headers_pending(), "pipeline remainder was lost");
    const auto &next = session.request();
    accepted = app.accept_headers({next.method, next.path, next.authorization});
    require(accepted && std::holds_alternative<tds::FeedHead>(*accepted),
            "previous identity or route leaked into pipelined read");
    session.accept_headers();
    require(session.consume().valid && session.complete(), "pipelined read did not complete");

    tds::http::HttpSession malformed;
    malformed.append("POST /posts/abc/like HTTP/1.1\r\nHost: local\r\nAuthorization: " + token +
                     "\r\nContent-Length: invalid\r\n\r\n");
    require(!malformed.consume().valid && !malformed.headers_pending(),
            "application ID validation preceded malformed framing");
}

void health_and_fail_fast_commit()
{
    Fixture fixture;
    tds::Application app(fixture.path, std::string(secret));
    const auto health = app.read(tds::ReadQuery{tds::Health{}});
    require(health.status == tds::Status::ok &&
                health.body.starts_with("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"),
            "health fields or ordering changed");

    tds::Application missing(fixture.path + ".missing", std::string(secret));
    std::array<tds::WriteCommand, 0> empty;
    std::array<tds::WriteOutcome, 0> no_outcomes;
    require(missing.commit(empty, no_outcomes).has_value(), "empty batch accessed the database");
    std::array<tds::WriteOutcome, 1> mismatched;
    bool rejected = false;
    try
    {
        static_cast<void>(missing.commit(empty, mismatched));
    }
    catch (const std::logic_error &)
    {
        rejected = true;
    }
    require(rejected, "invalid batch accessed the database before its precondition check");
    const auto degraded = missing.read(tds::ReadQuery{tds::Health{}});
    require(
        degraded.status == tds::Status::unavailable &&
            degraded.body.starts_with("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"),
        "missing database did not produce degraded health");
    const auto parsed = document(degraded.body);
    require(!tds::text_value(tds::member(yyjson_doc_get_root(parsed.get()), "error")).empty(),
            "degraded health lost its error detail");
}
} // namespace

int main()
{
    try
    {
        auth_and_validation_order();
        owned_create_and_unicode();
        fresh_reads_and_write_results();
        fragmented_context_and_pipeline();
        health_and_fail_fast_commit();
        std::puts("Application preparation, fresh reads, commit replies and HTTP context: 5 groups "
                  "passed");

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "Application component checks failed: %s\n", error.what());

        return 1;
    }
}
