#include <glaze/glaze.hpp>
#include <gtest/gtest.h>


// ReSharper disable once CppUseInternalLinkage
struct Post {
    int id;
    std::string body;
    std::string created_at;
    std::string author;
    int like_count;
};


TEST(Glaze, PostKeyOrderIsCompact) {
    std::string out;

    ASSERT_FALSE(
        glz::write_json(Post{.id = 1, .body = "hi", .created_at = "2025-12-31T23:59:19.409Z", .author = "bob", .
            like_count = 0}, out));

    EXPECT_EQ(out, R"({"id":1,"body":"hi","created_at":"2025-12-31T23:59:19.409Z","author":"bob","like_count":0})");
}
