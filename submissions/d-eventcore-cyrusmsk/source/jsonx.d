module jsonx;

import core.stdc.string : memcpy;

version (D_SIMD)
{
    import core.simd;
}

nothrow @nogc:

enum maxPostID = 9007199254740992L;

struct Buf
{
    nothrow @nogc:
    ubyte* data;
    size_t len;
    size_t cap;
    bool overflow;

    void init(ubyte[] storage)
    {
        data = storage.ptr;
        len = 0;
        cap = storage.length;
        overflow = false;
    }

    bool ensure(size_t extra)
    {
        if (extra > cap - len)
        {
            overflow = true;
            return false;
        }
        return true;
    }

    void putByte(char c)
    {
        if (len < cap)
            data[len++] = cast(ubyte) c;
        else
            overflow = true;
    }

    void put(const(char)[] s)
    {
        if (s.length)
        {
            if (s.length > cap - len)
            {
                overflow = true;
                return;
            }
            memcpy(data + len, s.ptr, s.length);
            len += s.length;
        }
    }
}

void putLong(ref Buf b, long value)
{
    char[20] buffer;
    size_t pos = buffer.length;
    bool negative = value < 0;
    ulong remaining = negative ? cast(ulong) -value : cast(ulong) value;

    do
    {
        buffer[--pos] = cast(char) ('0' + remaining % 10);
        remaining /= 10;
    }
    while (remaining);

    if (negative)
        buffer[--pos] = '-';

    b.put(buffer[pos .. $]);
}

version (D_SIMD)
{
    private bool needsEscape16(const(char)* p)
    {
        auto v = *cast(const ubyte16*) p;
        auto mask = (v == cast(ubyte16) 34) |
                    (v == cast(ubyte16) 92) |
                    (v < cast(ubyte16) 32);
        ulong[2] words = *cast(ulong[2]*) &mask;
        return (words[0] | words[1]) != 0;
    }
}

void putJsonString(ref Buf b, const(char)[] s)
{
    static immutable hex = "0123456789abcdef";

    b.putByte('"');

    size_t i;
    size_t runStart;

    while (i < s.length)
    {
        version (D_SIMD)
        {
            while (i + 16 <= s.length && !needsEscape16(s.ptr + i))
                i += 16;
        }

        while (i < s.length)
        {
            auto c = cast(ubyte) s[i];
            if (c == '"' || c == '\\' || c < 0x20)
                break;
            ++i;
        }

        if (i > runStart)
            b.put(s[runStart .. i]);

        if (i == s.length)
            break;

        auto c = cast(ubyte) s[i];
        switch (c)
        {
        case '"':
            b.put(`\"`);
            break;
        case '\\':
            b.put(`\\`);
            break;
        case '\b':
            b.put(`\b`);
            break;
        case '\f':
            b.put(`\f`);
            break;
        case '\n':
            b.put(`\n`);
            break;
        case '\r':
            b.put(`\r`);
            break;
        case '\t':
            b.put(`\t`);
            break;
        default:
            b.put(`\u00`);
            b.putByte(hex[(c >> 4) & 0xF]);
            b.putByte(hex[c & 0xF]);
            break;
        }

        ++i;
        runStart = i;
    }

    b.putByte('"');
}

private bool isWs(ubyte c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

private void skipWs(const(ubyte)[] data, ref size_t pos)
{
    while (pos < data.length && isWs(data[pos]))
        ++pos;
}

private int hexValue(ubyte c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

private bool readHex4(const(ubyte)[] data, ref size_t pos, out uint value)
{
    if (pos + 4 > data.length)
        return false;

    uint v;
    foreach (_; 0 .. 4)
    {
        int h = hexValue(data[pos++]);
        if (h < 0)
            return false;
        v = (v << 4) | cast(uint) h;
    }

    value = v;
    return true;
}

private bool appendCodepoint(ref Buf dest, uint cp)
{
    if (cp <= 0x7F)
    {
        if (!dest.ensure(1))
            return false;
        dest.data[dest.len++] = cast(ubyte) cp;
        return true;
    }
    if (cp <= 0x7FF)
    {
        if (!dest.ensure(2))
            return false;
        dest.data[dest.len++] = cast(ubyte) (0xC0 | (cp >> 6));
        dest.data[dest.len++] = cast(ubyte) (0x80 | (cp & 0x3F));
        return true;
    }
    if (cp <= 0xFFFF)
    {
        if (!dest.ensure(3))
            return false;
        dest.data[dest.len++] = cast(ubyte) (0xE0 | (cp >> 12));
        dest.data[dest.len++] = cast(ubyte) (0x80 | ((cp >> 6) & 0x3F));
        dest.data[dest.len++] = cast(ubyte) (0x80 | (cp & 0x3F));
        return true;
    }
    if (cp <= 0x10FFFF)
    {
        if (!dest.ensure(4))
            return false;
        dest.data[dest.len++] = cast(ubyte) (0xF0 | (cp >> 18));
        dest.data[dest.len++] = cast(ubyte) (0x80 | ((cp >> 12) & 0x3F));
        dest.data[dest.len++] = cast(ubyte) (0x80 | ((cp >> 6) & 0x3F));
        dest.data[dest.len++] = cast(ubyte) (0x80 | (cp & 0x3F));
        return true;
    }
    return false;
}

bool parseJsonString(const(ubyte)[] data, ref size_t pos, ref Buf dest)
{
    if (pos >= data.length || data[pos] != '"')
        return false;
    ++pos;

    while (pos < data.length)
    {
        ubyte c = data[pos++];

        if (c == '"')
            return true;

        if (c < 0x20)
            return false;

        if (c != '\\')
        {
            if (!dest.ensure(1))
                return false;
            dest.data[dest.len++] = c;
            continue;
        }

        if (pos >= data.length)
            return false;

        ubyte esc = data[pos++];
        uint cp;

        switch (esc)
        {
        case '"':
        case '\\':
        case '/':
            cp = esc;
            break;
        case 'b':
            cp = 0x08;
            break;
        case 'f':
            cp = 0x0C;
            break;
        case 'n':
            cp = 0x0A;
            break;
        case 'r':
            cp = 0x0D;
            break;
        case 't':
            cp = 0x09;
            break;
        case 'u':
            if (!readHex4(data, pos, cp))
                return false;
            if (cp >= 0xD800 && cp <= 0xDBFF)
            {
                if (pos + 6 > data.length || data[pos] != '\\' || data[pos + 1] != 'u')
                    return false;
                pos += 2;
                uint low;
                if (!readHex4(data, pos, low) || low < 0xDC00 || low > 0xDFFF)
                    return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF)
                return false;
            break;
        default:
            return false;
        }

        if (!appendCodepoint(dest, cp))
            return false;
    }

    return false;
}

private bool skipJsonString(const(ubyte)[] data, ref size_t pos)
{
    if (pos >= data.length || data[pos] != '"')
        return false;
    ++pos;

    while (pos < data.length)
    {
        ubyte c = data[pos++];
        if (c == '"')
            return true;
        if (c < 0x20)
            return false;
        if (c != '\\')
            continue;
        if (pos >= data.length)
            return false;
        ubyte esc = data[pos++];
        switch (esc)
        {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            break;
        case 'u':
        {
            uint cp;
            if (!readHex4(data, pos, cp))
                return false;
            if (cp >= 0xD800 && cp <= 0xDBFF)
            {
                if (pos + 6 > data.length || data[pos] != '\\' || data[pos + 1] != 'u')
                    return false;
                pos += 2;
                uint low;
                if (!readHex4(data, pos, low) || low < 0xDC00 || low > 0xDFFF)
                    return false;
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF)
                return false;
            break;
        }
        default:
            return false;
        }
    }

    return false;
}

bool parseJsonNumber(const(ubyte)[] data, ref size_t pos, out double value)
{
    size_t start = pos;
    bool negative;
    double mantissa = 0.0;
    int exponent;

    if (pos < data.length && data[pos] == '-')
    {
        negative = true;
        ++pos;
    }

    if (pos >= data.length)
        return false;

    if (data[pos] == '0')
    {
        ++pos;
        if (pos < data.length && data[pos] >= '0' && data[pos] <= '9')
            return false;
    }
    else if (data[pos] >= '1' && data[pos] <= '9')
    {
        while (pos < data.length && data[pos] >= '0' && data[pos] <= '9')
        {
            mantissa = mantissa * 10.0 + (data[pos] - '0');
            ++pos;
        }
    }
    else
        return false;

    if (pos < data.length && data[pos] == '.')
    {
        ++pos;
        if (pos >= data.length || data[pos] < '0' || data[pos] > '9')
            return false;
        double scale = 0.1;
        while (pos < data.length && data[pos] >= '0' && data[pos] <= '9')
        {
            mantissa += (data[pos] - '0') * scale;
            scale *= 0.1;
            ++pos;
        }
    }

    if (pos < data.length && (data[pos] == 'e' || data[pos] == 'E'))
    {
        ++pos;
        bool expNegative;
        if (pos < data.length && (data[pos] == '+' || data[pos] == '-'))
        {
            expNegative = data[pos] == '-';
            ++pos;
        }
        if (pos >= data.length || data[pos] < '0' || data[pos] > '9')
            return false;
        while (pos < data.length && data[pos] >= '0' && data[pos] <= '9')
        {
            if (exponent < 10000)
                exponent = exponent * 10 + (data[pos] - '0');
            ++pos;
        }
        if (expNegative)
            exponent = -exponent;
    }

    if (pos == start + (negative ? 1 : 0))
        return false;

    while (exponent > 0)
    {
        mantissa *= 10.0;
        --exponent;
    }
    while (exponent < 0)
    {
        mantissa *= 0.1;
        ++exponent;
    }

    value = negative ? -mantissa : mantissa;
    return true;
}

private bool skipJsonValue(const(ubyte)[] data, ref size_t pos, int depth)
{
    if (depth > 32)
        return false;

    skipWs(data, pos);
    if (pos >= data.length)
        return false;

    switch (data[pos])
    {
    case '"':
        return skipJsonString(data, pos);
    case '{':
    {
        ++pos;
        skipWs(data, pos);
        if (pos < data.length && data[pos] == '}')
        {
            ++pos;
            return true;
        }
        while (pos < data.length)
        {
            if (!skipJsonString(data, pos))
                return false;
            skipWs(data, pos);
            if (pos >= data.length || data[pos] != ':')
                return false;
            ++pos;
            if (!skipJsonValue(data, pos, depth + 1))
                return false;
            skipWs(data, pos);
            if (pos >= data.length)
                return false;
            if (data[pos] == '}')
            {
                ++pos;
                return true;
            }
            if (data[pos] != ',')
                return false;
            ++pos;
            skipWs(data, pos);
        }
        return false;
    }
    case '[':
    {
        ++pos;
        skipWs(data, pos);
        if (pos < data.length && data[pos] == ']')
        {
            ++pos;
            return true;
        }
        while (pos < data.length)
        {
            if (!skipJsonValue(data, pos, depth + 1))
                return false;
            skipWs(data, pos);
            if (pos >= data.length)
                return false;
            if (data[pos] == ']')
            {
                ++pos;
                return true;
            }
            if (data[pos] != ',')
                return false;
            ++pos;
            skipWs(data, pos);
        }
        return false;
    }
    case 't':
        if (pos + 4 <= data.length && data[pos .. pos + 4] == cast(const(ubyte)[]) "true")
        {
            pos += 4;
            return true;
        }
        return false;
    case 'f':
        if (pos + 5 <= data.length && data[pos .. pos + 5] == cast(const(ubyte)[]) "false")
        {
            pos += 5;
            return true;
        }
        return false;
    case 'n':
        if (pos + 4 <= data.length && data[pos .. pos + 4] == cast(const(ubyte)[]) "null")
        {
            pos += 4;
            return true;
        }
        return false;
    default:
    {
        double ignored;
        return parseJsonNumber(data, pos, ignored);
    }
    }
}

private bool sameKey(const(ubyte)[] key, const(char)[] expected)
{
    if (key.length != expected.length)
        return false;
    foreach (i; 0 .. key.length)
    {
        if (key[i] != cast(ubyte) expected[i])
            return false;
    }
    return true;
}

enum CreateBodyResult
{
    ok,
    malformed,
    missing,
    notString
}

CreateBodyResult parseCreateBody(const(ubyte)[] data, ref Buf body)
{
    size_t pos;
    skipWs(data, pos);

    if (pos >= data.length || data[pos] != '{')
        return CreateBodyResult.malformed;
    ++pos;
    skipWs(data, pos);

    bool present;
    bool isString;
    bool malformed;
    bool seen;

    if (pos < data.length && data[pos] == '}')
        ++pos;
    else
    {
        while (true)
        {
            ubyte[128] keyStorage;
            Buf key;
            key.init(keyStorage[]);

            if (!parseJsonString(data, pos, key))
                return CreateBodyResult.malformed;

            skipWs(data, pos);
            if (pos >= data.length || data[pos] != ':')
                return CreateBodyResult.malformed;
            ++pos;
            skipWs(data, pos);

            if (sameKey(key.data[0 .. key.len], "body"))
            {
                present = true;
                seen = true;

                if (pos < data.length && data[pos] == '"')
                {
                    isString = true;
                    body.len = 0;
                    if (!parseJsonString(data, pos, body))
                        malformed = true;
                }
                else
                {
                    isString = false;
                    if (!skipJsonValue(data, pos, 0))
                        malformed = true;
                }
            }
            else if (!skipJsonValue(data, pos, 0))
                return CreateBodyResult.malformed;

            if (malformed)
                return CreateBodyResult.malformed;

            skipWs(data, pos);
            if (pos >= data.length)
                return CreateBodyResult.malformed;

            if (data[pos] == '}')
            {
                ++pos;
                break;
            }
            if (data[pos] != ',')
                return CreateBodyResult.malformed;
            ++pos;
            skipWs(data, pos);
        }
    }

    skipWs(data, pos);
    if (pos != data.length)
        return CreateBodyResult.malformed;

    if (!present || !seen)
        return CreateBodyResult.missing;
    if (!isString)
        return CreateBodyResult.notString;
    return CreateBodyResult.ok;
}

enum JwtPayloadResult
{
    ok,
    malformed,
    badPayload,
    badExpiry
}

JwtPayloadResult parseJwtPayload(
    const(ubyte)[] data,
    ref Buf sub,
    ref Buf username,
    out double expires
)
{
    size_t pos;
    skipWs(data, pos);

    if (pos >= data.length || data[pos] != '{')
        return JwtPayloadResult.malformed;
    ++pos;
    skipWs(data, pos);

    bool hasSub;
    bool hasUsername;
    bool hasExp;
    bool badPayload;
    bool badExpiry;

    if (pos < data.length && data[pos] == '}')
        ++pos;
    else
    {
        while (true)
        {
            ubyte[128] keyStorage;
            Buf key;
            key.init(keyStorage[]);

            if (!parseJsonString(data, pos, key))
                return JwtPayloadResult.malformed;

            skipWs(data, pos);
            if (pos >= data.length || data[pos] != ':')
                return JwtPayloadResult.malformed;
            ++pos;
            skipWs(data, pos);

            if (sameKey(key.data[0 .. key.len], "sub"))
            {
                if (pos < data.length && data[pos] == '"')
                {
                    sub.len = 0;
                    if (!parseJsonString(data, pos, sub))
                        return JwtPayloadResult.malformed;
                    hasSub = true;
                }
                else
                {
                    badPayload = true;
                    if (!skipJsonValue(data, pos, 0))
                        return JwtPayloadResult.malformed;
                }
            }
            else if (sameKey(key.data[0 .. key.len], "username"))
            {
                if (pos < data.length && data[pos] == '"')
                {
                    username.len = 0;
                    if (!parseJsonString(data, pos, username))
                        return JwtPayloadResult.malformed;
                    hasUsername = true;
                }
                else
                {
                    badPayload = true;
                    if (!skipJsonValue(data, pos, 0))
                        return JwtPayloadResult.malformed;
                }
            }
            else if (sameKey(key.data[0 .. key.len], "exp"))
            {
                if (pos < data.length &&
                    (data[pos] == '-' || (data[pos] >= '0' && data[pos] <= '9')))
                {
                    if (!parseJsonNumber(data, pos, expires))
                        return JwtPayloadResult.malformed;
                    hasExp = true;
                }
                else
                {
                    badExpiry = true;
                    if (!skipJsonValue(data, pos, 0))
                        return JwtPayloadResult.malformed;
                }
            }
            else if (!skipJsonValue(data, pos, 0))
                return JwtPayloadResult.malformed;

            skipWs(data, pos);
            if (pos >= data.length)
                return JwtPayloadResult.malformed;
            if (data[pos] == '}')
            {
                ++pos;
                break;
            }
            if (data[pos] != ',')
                return JwtPayloadResult.malformed;
            ++pos;
            skipWs(data, pos);
        }
    }

    skipWs(data, pos);
    if (pos != data.length)
        return JwtPayloadResult.malformed;

    if (badPayload)
        return JwtPayloadResult.badPayload;
    if (!hasSub || !hasUsername)
        return JwtPayloadResult.badPayload;
    if (!hasExp || badExpiry)
        return JwtPayloadResult.badExpiry;
    return JwtPayloadResult.ok;
}

bool parsePositiveId(const(char)[] text, out long id)
{
    if (!text.length)
        return false;

    ulong parsed;
    foreach (char c; text)
    {
        if (c < '0' || c > '9')
            return false;
        auto digit = cast(ulong) (c - '0');
        if (parsed > (cast(ulong) maxPostID - digit) / 10)
            return false;
        parsed = parsed * 10 + digit;
    }

    if (parsed == 0)
        return false;

    id = cast(long) parsed;
    return true;
}

size_t codepointCount(const(char)[] text)
{
    size_t count;
    foreach (char c; text)
    {
        if ((cast(ubyte) c & 0xC0) != 0x80)
            ++count;
    }
    return count;
}

const(char)[] trimAscii(const(char)[] text)
{
    size_t start;
    size_t end = text.length;

    while (start < end)
    {
        auto c = text[start];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        ++start;
    }

    while (end > start)
    {
        auto c = text[end - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        --end;
    }

    return text[start .. end];
}

private uint rotr(uint x, uint n)
{
    return (x >> n) | (x << (32 - n));
}

private static immutable uint[64] shaK = [
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5,
    0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3,
    0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC,
    0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7,
    0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13,
    0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3,
    0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5,
    0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208,
    0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
];

private void shaTransform(ref uint[8] state, const(ubyte)* block)
{
    uint[64] w;

    foreach (i; 0 .. 16)
    {
        w[i] = (cast(uint) block[i * 4] << 24) |
               (cast(uint) block[i * 4 + 1] << 16) |
               (cast(uint) block[i * 4 + 2] << 8) |
               cast(uint) block[i * 4 + 3];
    }

    foreach (i; 16 .. 64)
    {
        uint s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint a = state[0];
    uint b = state[1];
    uint c = state[2];
    uint d = state[3];
    uint e = state[4];
    uint f = state[5];
    uint g = state[6];
    uint h = state[7];

    foreach (i; 0 .. 64)
    {
        uint s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint ch = (e & f) ^ (~e & g);
        uint temp1 = h + s1 + ch + shaK[i] + w[i];
        uint s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint maj = (a & b) ^ (a & c) ^ (b & c);
        uint temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

struct Sha256
{
    nothrow @nogc:
    uint[8] state;
    ubyte[64] block;
    size_t blockLen;
    ulong total;

    void init()
    {
        state = [
            0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
            0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
        ];
        blockLen = 0;
        total = 0;
    }

    void update(const(ubyte)[] data)
    {
        total += data.length;
        size_t pos;

        if (blockLen)
        {
            size_t need = 64 - blockLen;
            size_t take = data.length < need ? data.length : need;
            memcpy(block.ptr + blockLen, data.ptr, take);
            blockLen += take;
            pos += take;

            if (blockLen == 64)
            {
                shaTransform(state, block.ptr);
                blockLen = 0;
            }
        }

        while (pos + 64 <= data.length)
        {
            shaTransform(state, data.ptr + pos);
            pos += 64;
        }

        if (pos < data.length)
        {
            size_t remaining = data.length - pos;
            memcpy(block.ptr, data.ptr + pos, remaining);
            blockLen = remaining;
        }
    }

    void finish(ubyte[32]* digest)
    {
        ulong bits = total * 8;
        block[blockLen++] = 0x80;

        if (blockLen > 56)
        {
            while (blockLen < 64)
                block[blockLen++] = 0;
            shaTransform(state, block.ptr);
            blockLen = 0;
        }

        while (blockLen < 56)
            block[blockLen++] = 0;

        foreach (i; 0 .. 8)
            block[56 + i] = cast(ubyte) (bits >> (56 - i * 8));

        shaTransform(state, block.ptr);

        foreach (i; 0 .. 8)
        {
            uint v = state[i];
            digest[0][i * 4] = cast(ubyte) (v >> 24);
            digest[0][i * 4 + 1] = cast(ubyte) (v >> 16);
            digest[0][i * 4 + 2] = cast(ubyte) (v >> 8);
            digest[0][i * 4 + 3] = cast(ubyte) v;
        }
    }
}

struct HmacSha256
{
    nothrow @nogc:
    ubyte[64] innerPad;
    ubyte[64] outerPad;

    void init(const(char)[] secret)
    {
        ubyte[64] key;
        ubyte[32] digest;

        if (secret.length > 64)
        {
            Sha256 hash;
            hash.init();
            hash.update(cast(const(ubyte)[]) secret);
            hash.finish(&digest);
            memcpy(key.ptr, digest.ptr, 32);
        }
        else if (secret.length)
            memcpy(key.ptr, secret.ptr, secret.length);

        foreach (i; 0 .. 64)
        {
            innerPad[i] = key[i] ^ 0x36;
            outerPad[i] = key[i] ^ 0x5C;
        }
    }

    void sign(const(ubyte)[] part1, const(ubyte)[] part2, ubyte[32]* digest)
    {
        ubyte[32] inner;
        Sha256 hash;

        hash.init();
        hash.update(innerPad[]);
        hash.update(part1);
        hash.update(part2);
        hash.finish(&inner);

        hash.init();
        hash.update(outerPad[]);
        hash.update(inner[]);
        hash.finish(digest);
    }
}

private bool b64Value(char c, out ubyte value)
{
    if (c >= 'A' && c <= 'Z')
    {
        value = cast(ubyte) (c - 'A');
        return true;
    }
    if (c >= 'a' && c <= 'z')
    {
        value = cast(ubyte) (c - 'a' + 26);
        return true;
    }
    if (c >= '0' && c <= '9')
    {
        value = cast(ubyte) (c - '0' + 52);
        return true;
    }
    if (c == '-')
    {
        value = 62;
        return true;
    }
    if (c == '_')
    {
        value = 63;
        return true;
    }
    return false;
}

bool base64UrlDecode(const(char)[] input, ubyte[] output, out size_t outputLen)
{
    size_t pos;
    size_t outPos;
    uint accumulator;
    int bits;
    bool sawPadding;

    while (pos < input.length)
    {
        char c = input[pos++];

        if (c == '=')
        {
            sawPadding = true;
            continue;
        }

        if (sawPadding)
            return false;

        ubyte v;
        if (!b64Value(c, v))
            return false;

        accumulator = (accumulator << 6) | v;
        bits += 6;

        if (bits >= 8)
        {
            bits -= 8;
            if (outPos >= output.length)
                return false;
            output[outPos++] = cast(ubyte) (accumulator >> bits);
        }
    }

    if (bits >= 6)
        return false;
    if (accumulator & ((1u << bits) - 1))
        return false;

    outputLen = outPos;
    return true;
}

bool constantTimeEqual(const(ubyte)[] a, const(ubyte)[] b)
{
    if (a.length != b.length)
        return false;
    uint diff;
    foreach (i; 0 .. a.length)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
