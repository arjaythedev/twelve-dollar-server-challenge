#include "post_writer.hpp"
#include "../json.hpp"

namespace tds
{

PostWriter::PostWriter(std::string &output, PostShape shape) : output_(output), shape_(shape)
{
    reset();
    if (shape_ == PostShape::feed)
    {
        output_.reserve(8192);
    }
}

void PostWriter::append(PostId id, std::string_view body, std::string_view timestamp,
                        std::string_view author, std::int64_t likes)
{
    if (shape_ == PostShape::feed && !first_)
    {
        output_ += ',';
    }
    first_ = false;
    output_ += "{\"id\":";
    integer(output_, id.value);
    output_ += ",\"body\":";
    quote(output_, body);
    output_ += ",\"created_at\":";
    quote(output_, timestamp);
    output_ += ",\"author\":";
    quote(output_, author);
    output_ += ",\"like_count\":";
    integer(output_, likes);
    output_ += '}';
}

void PostWriter::reset()
{
    // Discard partial B-tree output before SQL fallback reuses the same response buffer
    if (shape_ == PostShape::feed)
    {
        output_ = "{\"posts\":[";
    }
    else
    {
        output_ = "{\"post\":";
    }
    first_ = true;
}

void PostWriter::finish()
{
    // Slightly cursed: append closes each post, this closes only the outer JSON envelope
    if (shape_ == PostShape::feed)
    {
        output_ += "]}";
    }
    else
    {
        output_ += '}';
    }
}

} // namespace tds
