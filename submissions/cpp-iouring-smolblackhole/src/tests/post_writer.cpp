#include "app/post_writer.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

void require(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void empty_feed()
{
    std::string output = "previous response";
    tds::PostWriter writer(output, tds::PostShape::feed);
    writer.finish();
    require(output == "{\"posts\":[]}", "empty feed envelope changed");
}

void single_post_bytes()
{
    std::string output;
    tds::PostWriter writer(output, tds::PostShape::single);
    std::string body = "quote \" slash \\ line\n\t\r\b\f\x01 caf\xc3\xa9";
    writer.append(tds::PostId{42}, body, "2026-11-02T00:00:00.123Z", "a\"b", 7);
    body.assign(body.size(), 'x');
    writer.finish();
    const std::string_view expected =
        "{\"post\":{\"id\":42,\"body\":\"quote \\\" slash \\\\ line\\n\\t\\r\\b\\f\\u0001 "
        "caf\xc3\xa9\","
        "\"created_at\":\"2026-11-02T00:00:00.123Z\",\"author\":\"a\\\"b\",\"like_count\":7}}";
    require(output == expected, "post key order, escaping or borrowed-view lifetime changed");
}

void feed_order_and_fallback()
{
    std::string output;
    tds::PostWriter writer(output, tds::PostShape::feed);
    writer.append(tds::PostId{999}, "discarded partial B-tree row", "bad", "old", 999);
    writer.reset();
    writer.append(tds::PostId{2}, "newer", "2026-11-02T00:00:00.001Z", "second", 0);
    writer.append(tds::PostId{1}, "older", "2026-11-02T00:00:00.000Z", "first", 3);
    writer.finish();
    const std::string_view expected =
        "{\"posts\":[{\"id\":2,\"body\":\"newer\",\"created_at\":\"2026-11-02T00:00:00.001Z\","
        "\"author\":\"second\",\"like_count\":0},{\"id\":1,\"body\":\"older\","
        "\"created_at\":\"2026-11-02T00:00:00.000Z\",\"author\":\"first\",\"like_count\":3}]}";
    require(output == expected, "feed fallback retained partial rows or changed row order");
}

} // namespace

int main()
{
    try
    {
        empty_feed();
        single_post_bytes();
        feed_order_and_fallback();
        std::puts("Post writer bytes: 3 groups passed");

        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "Post writer bytes failed: %s\n", error.what());

        return 1;
    }
}
