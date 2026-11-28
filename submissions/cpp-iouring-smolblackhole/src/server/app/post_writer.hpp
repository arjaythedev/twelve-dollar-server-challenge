#pragma once

#include "../model.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tds
{

enum class PostShape
{
    feed,
    single
};

class PostWriter
{
  public:
    PostWriter(std::string &output, PostShape shape);

    // SQLite views are consumed before append returns
    void append(PostId id, std::string_view body, std::string_view timestamp,
                std::string_view author, std::int64_t likes);
    void reset();
    void finish();

  private:
    std::string &output_;
    PostShape shape_;
    bool first_ = true;
};

} // namespace tds
