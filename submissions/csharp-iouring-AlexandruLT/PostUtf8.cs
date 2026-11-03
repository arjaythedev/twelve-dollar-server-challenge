using System.Buffers;
using System.Text;
using System.Text.Json;

namespace Feed;
static class PostUtf8
{
    public static string? Parse(ReadOnlySpan<byte> input, Span<byte> destination, out int offset, out int length)
    {
        offset = length = 0; bool found = false;
        JsonInput.Validate(input);
        var reader = new Utf8JsonReader(input);
        if (!reader.Read()) throw new JsonException();
        if (reader.TokenType != JsonTokenType.StartObject)
        {
            reader.Skip(); if (reader.Read()) throw new JsonException(); return "body is required";
        }
        while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
        {
            if (reader.TokenType != JsonTokenType.PropertyName) throw new JsonException();
            bool match = reader.ValueTextEquals("body"u8);
            if (!reader.Read()) throw new JsonException();
            if (match)
            {
                found = reader.TokenType == JsonTokenType.String;
                length = found ? reader.CopyString(destination) : 0;
            }
            reader.Skip();
        }
        if (reader.TokenType != JsonTokenType.EndObject || reader.Read()) throw new JsonException();
        if (!found) return "body is required";
        while (offset < length)
        {
            if (Rune.DecodeFromUtf8(destination.Slice(offset, length - offset), out Rune rune, out int used) != OperationStatus.Done) throw new JsonException();
            if (!Trim(rune.Value)) break;
            offset += used;
        }
        while (length > offset)
        {
            if (Rune.DecodeLastFromUtf8(destination.Slice(offset, length - offset), out Rune rune, out int used) != OperationStatus.Done) throw new JsonException();
            if (!Trim(rune.Value)) break;
            length -= used;
        }
        length -= offset;
        if (length == 0) return "body is required";
        int runes = 0, position = offset;
        while (position < offset + length)
        {
            if (Rune.DecodeFromUtf8(destination.Slice(position, offset + length - position), out _, out int used) != OperationStatus.Done) throw new JsonException();
            position += used; if (++runes > 500) return "body must be at most 500 characters";
        }
        return null;
    }
    static bool Trim(int value) => value is >= 9 and <= 13 or 0x20 or 0xA0 or 0x1680 or >= 0x2000 and <= 0x200A
        or 0x2028 or 0x2029 or 0x202F or 0x205F or 0x3000 or 0xFEFF;
}
