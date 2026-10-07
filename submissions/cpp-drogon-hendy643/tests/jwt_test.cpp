#include <gtest/gtest.h>

#include "jwt.hpp"

namespace {
    constexpr std::string_view kSecret = "twelve-dollar-challenge";
    constexpr int64_t kNow = 1'700'000'000;

    std::string sign(std::string_view header, std::string_view payload, std::string_view secret = kSecret) {
        const std::string signing = jwt::b64url_encode(header) + '.' + jwt::b64url_encode(payload);
        return "Bearer " + signing + '.' + jwt::hmac_sha256_b64url(secret, signing);
    }

    constexpr std::string_view kHs256 = R"({"alg":"HS256","typ":"JWT"})";

    jwt::Status check(const std::string &auth, jwt::Identity &id) { return jwt::verify(auth, kSecret, kNow, id); }
}

TEST(Jwt, ValidToken) {
    jwt::Identity id;
    EXPECT_EQ(check(sign(kHs256, R"({"sub":"42","username":"bob","iat":1,"exp":1700003600})"), id), jwt::Status::Ok);
    EXPECT_EQ(id.user_id, 42);
    EXPECT_EQ(id.username, "bob");
}

TEST(Jwt, Rejections) {
    jwt::Identity id;
    const std::string good = R"({"sub":"1","username":"a","exp":1700003600})";
    EXPECT_EQ(check("Basic abc", id), jwt::Status::MissingBearer);
    EXPECT_EQ(check("", id), jwt::Status::MissingBearer);
    EXPECT_EQ(check("Bearer abc", id), jwt::Status::InvalidToken);
    EXPECT_EQ(check(sign(kHs256, good, "other-secret"), id), jwt::Status::InvalidToken);
    EXPECT_EQ(check(sign(R"({"alg":"none"})", good), id), jwt::Status::InvalidToken);
    EXPECT_EQ(check(sign(kHs256, R"({"sub":"1","username":"a","exp":1699999999})"), id), jwt::Status::InvalidToken);
    EXPECT_EQ(check(sign(kHs256, R"({"sub":"x","username":"a","exp":1699999999})"), id), jwt::Status::InvalidToken);
}

TEST(Jwt, BadPayload) {
    jwt::Identity id;
    for (const char *p: {
             R"({"sub":"abc","username":"a","exp":1700003600})", R"({"sub":"0","username":"a"})",
             R"({"sub":"-1","username":"a"})", R"({"sub":5,"username":"a"})", R"({"sub":"1"})",
             R"({"sub":"1","username":7})"
         })
        EXPECT_EQ(check(sign(kHs256, p), id), jwt::Status::InvalidPayload) << p;
}
