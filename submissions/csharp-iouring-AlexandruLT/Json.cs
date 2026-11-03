using System.Buffers;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace Feed;

static class JsonInput
{
    static readonly UTF8Encoding StrictUtf8 = new(false, true);
    public static void Validate(ReadOnlySpan<byte> bytes) => StrictUtf8.GetCharCount(bytes);
}

// Writes response bodies from byte templates in the exact key order the spec requires.
// Every value comes from SQLite or the request; strings go through the platform's escaping.
sealed class Output(int initialCapacity = 8192)
{
    public readonly ArrayBufferWriter<byte> Buffer = new(initialCapacity);
    Utf8JsonWriter? writer;
    Utf8JsonWriter Writer => writer ??= new(Buffer, new JsonWriterOptions { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
    public void Reset() => Buffer.Clear();
    public void Raw(ReadOnlySpan<byte> bytes) { bytes.CopyTo(Buffer.GetSpan(bytes.Length)); Buffer.Advance(bytes.Length); }
    public void Number(long value)
    {
        System.Buffers.Text.Utf8Formatter.TryFormat(value, Buffer.GetSpan(32), out int n); Buffer.Advance(n);
    }
    public void Text(ReadOnlySpan<byte> bytes)
    {
        Writer.WriteStringValue(bytes); Writer.Flush(); Writer.Reset(Buffer);
    }
    public void Post(long id, ReadOnlySpan<byte> body, ReadOnlySpan<byte> created, ReadOnlySpan<byte> author, long likes)
    {
        Raw("{\"id\":"u8); Number(id); Raw(",\"body\":"u8); Text(body);
        Raw(",\"created_at\":"u8); Text(created); Raw(",\"author\":"u8); Text(author);
        Raw(",\"like_count\":"u8); Number(likes); Raw("}"u8);
    }
    public void Begin(bool feed) => Raw(feed ? "{\"posts\":["u8 : "{\"post\":"u8);
    public void End(bool feed) => Raw(feed ? "]}"u8 : "}"u8);
}
