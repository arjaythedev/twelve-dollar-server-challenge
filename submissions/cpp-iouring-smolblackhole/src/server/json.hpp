#pragma once

#include <yyjson.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace tds
{

struct DocumentDeleter
{
    void operator()(yyjson_doc *doc) const noexcept
    {
        yyjson_doc_free(doc);
    }
};

using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;
enum class JSONError
{
    malformed
};
std::expected<Document, JSONError> read_json(std::string_view text);
yyjson_val *member(yyjson_val *object, std::string_view key) noexcept;
std::string_view text_value(yyjson_val *value) noexcept;
void quote(std::string &output, std::string_view text);
void integer(std::string &output, std::int64_t value);

} // namespace tds
