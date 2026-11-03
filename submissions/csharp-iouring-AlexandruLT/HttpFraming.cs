using System.Buffers.Text;

namespace Feed;

struct RequestHead
{
    public int HeaderLength, PathOffset, PathLength, AuthOffset, AuthLength, ContentLength;
    public bool Get, Post, Chunked, Close, Continue;
}

static class HttpFraming
{
    public const int HeaderLimit = 32768, BodyLimit = 16384, WireLimit = 65536;
    public static bool EqualAscii(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b)
    {
        if (a.Length != b.Length) return false;
        for (int n = 0; n < a.Length; n++)
        {
            int x = a[n], y = b[n];
            if (x is >= 65 and <= 90) x += 32;
            if (y is >= 65 and <= 90) y += 32;
            if (x != y) return false;
        }
        return true;
    }
    public static ReadOnlySpan<byte> Trim(ReadOnlySpan<byte> value)
    {
        while (!value.IsEmpty && value[0] is 32 or 9) value = value[1..];
        while (!value.IsEmpty && value[^1] is 32 or 9) value = value[..^1];
        return value;
    }
    static bool Token(ReadOnlySpan<byte> value)
    {
        if (value.IsEmpty) return false;
        foreach (byte b in value)
            if (!(b is >= 65 and <= 90 or >= 97 and <= 122 or >= 48 and <= 57) && !"!#$%&'*+-.^_`|~"u8.Contains(b)) return false;
        return true;
    }
    public static bool ValidHeader(ReadOnlySpan<byte> line)
    {
        int colon = line.IndexOf((byte)':');
        if (colon <= 0 || !Token(line[..colon])) return false;
        foreach (byte b in line[(colon + 1)..]) if (b < 32 && b != 9 || b == 127) return false;
        return true;
    }
    static bool ContainsToken(ReadOnlySpan<byte> value, ReadOnlySpan<byte> token)
    {
        while (!value.IsEmpty)
        {
            int comma = value.IndexOf((byte)',');
            if (EqualAscii(Trim(comma < 0 ? value : value[..comma]), token)) return true;
            if (comma < 0) return false;
            value = value[(comma + 1)..];
        }
        return false;
    }
    public static int Parse(ReadOnlySpan<byte> bytes, int headerLength, out RequestHead head)
    {
        head = new RequestHead { HeaderLength = headerLength };
        int lineEnd = bytes.IndexOf("\r\n"u8);
        if (lineEnd < 0 || lineEnd > 8192) return 400;
        ReadOnlySpan<byte> line = bytes[..lineEnd];
        int first = line.IndexOf((byte)' ');
        if (first <= 0 || !Token(line[..first])) return 400;
        int second = line[(first + 1)..].IndexOf((byte)' ');
        if (second <= 0) return 400;
        second += first + 1;
        ReadOnlySpan<byte> version = line[(second + 1)..];
        bool http11 = version.SequenceEqual("HTTP/1.1"u8);
        if (!http11 && !version.SequenceEqual("HTTP/1.0"u8)) return 400;
        head.Close = !http11;
        head.Get = line[..first].SequenceEqual("GET"u8); head.Post = line[..first].SequenceEqual("POST"u8);
        head.PathOffset = first + 1; head.PathLength = second - first - 1;
        ReadOnlySpan<byte> target = line.Slice(head.PathOffset, head.PathLength);
        if (target.IsEmpty || target[0] != (byte)'/') return 400;
        foreach (byte b in target) if (b < 33 || b == 127) return 400;
        int query = target.IndexOf((byte)'?'); if (query >= 0) head.PathLength = query;
        bool host = false, contentLength = false, transfer = false, auth = false, close = false, keep = false;
        int position = lineEnd + 2;
        while (position < headerLength - 2)
        {
            int length = bytes[position..headerLength].IndexOf("\r\n"u8);
            if (length < 0) return 400;
            line = bytes.Slice(position, length);
            if (!ValidHeader(line)) return 400;
            int colon = line.IndexOf((byte)':');
            ReadOnlySpan<byte> name = line[..colon], value = Trim(line[(colon + 1)..]);
            if (EqualAscii(name, "host"u8)) { if (host || value.IsEmpty) return 400; host = true; }
            else if (EqualAscii(name, "content-length"u8))
            {
                if (contentLength || transfer || value.IsEmpty) return 400;
                foreach (byte b in value) if (b is < 48 or > 57) return 400;
                if (!Utf8Parser.TryParse(value, out int count, out int used) || used != value.Length || count < 0) return 400;
                contentLength = true; head.ContentLength = count;
            }
            else if (EqualAscii(name, "transfer-encoding"u8))
            {
                if (transfer || contentLength || !EqualAscii(value, "chunked"u8)) return 400;
                transfer = head.Chunked = true;
            }
            else if (EqualAscii(name, "authorization"u8))
            {
                if (auth) return 400; auth = true;
                int start = colon + 1; while (start < line.Length && line[start] is 32 or 9) start++;
                head.AuthOffset = position + start; head.AuthLength = value.Length;
            }
            else if (EqualAscii(name, "connection"u8))
            { close |= ContainsToken(value, "close"u8); keep |= ContainsToken(value, "keep-alive"u8); }
            else if (EqualAscii(name, "expect"u8))
            { if (!EqualAscii(value, "100-continue"u8)) return 400; head.Continue = true; }
            position += length + 2;
        }
        if (http11 && !host) return 400;
        head.Close = close || !http11 && !keep;
        return 0;
    }
}

struct ChunkDecoder
{
    int stage, remaining, trailerBytes;
    public int Written;
    // cursor is in the connection's wire buffer. The caller may compact consumed
    // body bytes back to HeaderLength after an incomplete result, preserving headers.
    public int Consume(ReadOnlySpan<byte> wire, Span<byte> body, ref int cursor)
    {
        while (true)
        {
            if (stage is 0 or 3)
            {
                int length = wire[cursor..].IndexOf("\r\n"u8);
                if (length < 0) return wire.Length - cursor > 8192 ? 400 : 0;
                ReadOnlySpan<byte> line = wire.Slice(cursor, length); cursor += length + 2;
                if (stage == 3)
                {
                    trailerBytes += length + 2;
                    if (trailerBytes > HttpFraming.HeaderLimit) return 400;
                    if (length == 0) return 1;
                    if (!HttpFraming.ValidHeader(line)) return 400;
                    ReadOnlySpan<byte> name = line[..line.IndexOf((byte)':')];
                    if (HttpFraming.EqualAscii(name, "content-length"u8) || HttpFraming.EqualAscii(name, "transfer-encoding"u8)) return 400;
                    continue;
                }
                int semicolon = line.IndexOf((byte)';');
                ReadOnlySpan<byte> digits = semicolon < 0 ? line : line[..semicolon];
                if (digits.IsEmpty || digits.Length > 8) return 400;
                long size = 0;
                foreach (byte b in digits)
                {
                    int digit = b is >= 48 and <= 57 ? b - 48 : b is >= 65 and <= 70 ? b - 55 : b is >= 97 and <= 102 ? b - 87 : -1;
                    if (digit < 0) return 400;
                    size = size * 16 + digit;
                }
                if (size > HttpFraming.BodyLimit - Written) return 413;
                if (size == 0) { stage = 3; continue; }
                remaining = (int)size; stage = 1;
            }
            if (stage == 1)
            {
                int available = Math.Min(remaining, wire.Length - cursor);
                wire.Slice(cursor, available).CopyTo(body.Slice(Written, available));
                cursor += available; Written += available; remaining -= available;
                if (remaining != 0) return 0;
                stage = 2;
            }
            if (stage == 2)
            {
                if (wire.Length - cursor < 2) return 0;
                if (!wire.Slice(cursor, 2).SequenceEqual("\r\n"u8)) return 400;
                cursor += 2; stage = 0;
            }
        }
    }
}
