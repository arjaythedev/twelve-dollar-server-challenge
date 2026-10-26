using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace Feed;

/// <summary>
/// Routes one request, checks it and writes the JSON body. The status/error order follows the
/// C# entries by maks-march (PR #27) and gichamba (PR #74), with the token expiry rule from the
/// spec enforced here.
/// </summary>
internal sealed class Api(Db db, Jwt jwt, Writer writer) {
    public readonly Writer Writer = writer;

    /// <summary>Returned by Handle when the request was queued to the Writer: its response comes later.</summary>
    public const int Deferred = 0;

    // Where a deferred request's response goes. Set by the worker before each Handle call.
    public Worker Owner = null!;
    public int Slot;
    public uint Gen;
    public bool Close;

    // JavaScript's String.prototype.trim() whitespace set, so bodies trim like the reference.
    private const string JsWhitespace =
        "\t\n\v\f\r \u00A0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200A\u2028\u2029\u202F\u205F\u3000\uFEFF";

    private readonly long _started = Stopwatch.GetTimestamp();
    private char[] _chars = new char[16 * 1024];

    /// <summary>
    /// Handles one request, writes its JSON body to <paramref name="o"/> and returns the HTTP status.
    /// Any query string is ignored. Unknown routes and methods get 404.
    /// </summary>
    public int Handle(ReadOnlySpan<byte> method, ReadOnlySpan<byte> path, ReadOnlySpan<byte> authorization,
        bool hasAuthorization, ReadOnlySpan<byte> body, Out o) {
        int q = path.IndexOf((byte)'?');
        if (q >= 0) {
            path = path[..q];
        }

        if (method.SequenceEqual("GET"u8)) {
            if (path.SequenceEqual("/feed"u8)) {
                int start = o.Len;
                if (db.Feed(o)) {
                    return 200;
                }

                o.Len = start; // drop the partial list before writing the error
                return Error(o, 500, Internal);
            }
            if (path.SequenceEqual("/health"u8)) {
                return Health(o);
            }

            if (path.StartsWith("/posts/"u8) && path.Length > 7 && path[7..].IndexOf((byte)'/') < 0) {
                return ParseId(path[7..], out long id) switch {
                    IdResult.Invalid => Error(o, 400, InvalidId),
                    IdResult.TooLarge => Error(o, 404, PostNotFound),
                    _ => PostOrError(id, o),
                };
            }
        } else if (method.SequenceEqual("POST"u8)) {
            if (path.SequenceEqual("/posts"u8)) {
                AuthStatus auth = jwt.Verify(authorization, hasAuthorization);
                return auth == AuthStatus.Ok ? Create(body, o) : AuthError(o, auth);
            }
            if (path.StartsWith("/posts/"u8) && path.EndsWith("/like"u8) && path.Length > 12
                && path[7..^5].IndexOf((byte)'/') < 0) {
                // Auth first, then the id (per the spec).
                AuthStatus auth = jwt.Verify(authorization, hasAuthorization);
                if (auth != AuthStatus.Ok) {
                    return AuthError(o, auth);
                }

                return ParseId(path[7..^5], out long id) switch {
                    IdResult.Invalid => Error(o, 400, InvalidId),
                    IdResult.TooLarge => Error(o, 404, PostNotFound),
                    _ => Like(id, o),
                };
            }
        }
        return Error(o, 404, NotFound);
    }

    /// <summary>Writes the post, or the 404 or 500 error body.</summary>
    private int PostOrError(long id, Out o) {
        int status = db.Post(id, o);
        return status == 200 ? 200 : Error(o, status, status == 404 ? PostNotFound : Internal);
    }

    /// <summary>200 when SELECT 1 works, 503 with the SQLite error otherwise.</summary>
    private int Health(Out o) {
        string? error = db.Health();
        if (error is null) {
            o.Bytes("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"u8);
            o.Int((Stopwatch.GetTimestamp() - _started) / Stopwatch.Frequency);
            o.Byte((byte)'}');
            return 200;
        }
        o.Bytes("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"u8);
        o.JsonString(Encoding.UTF8.GetBytes(error));
        o.Byte((byte)'}');
        return 503;
    }

    /// <summary>Validates the JSON body (non-empty and at most 500 characters after trimming) and queues the post. The Writer answers 201.</summary>
    private int Create(ReadOnlySpan<byte> json, Out o) {
        switch (ReadBody(json, out int length)) {
            case BodyResult.Malformed: return Error(o, 400, Malformed);
            case BodyResult.Required: return Error(o, 400, BodyRequired);
        }

        ReadOnlySpan<char> trimmed = _chars.AsSpan(0, length).Trim(JsWhitespace);
        if (trimmed.IsEmpty) {
            return Error(o, 400, BodyRequired);
        }

        if (Characters(trimmed) > 500) {
            return Error(o, 400, BodyTooLong);
        }

        WriteJob job = NewJob(WriteJob.KindCreate);
        job.BodyLength = Encoding.UTF8.GetBytes(trimmed, job.Body);
        jwt.Username.CopyTo(job.Username);
        job.UsernameLength = jwt.Username.Length;
        Writer.Submit(job);
        return Deferred;
    }

    /// <summary>A write job for the authenticated user, addressed to the current connection.</summary>
    private WriteJob NewJob(byte kind) {
        WriteJob job = Writer.Rent();
        job.Kind = kind;
        job.UserId = jwt.UserId;
        job.Owner = Owner;
        job.Slot = Slot;
        job.Gen = Gen;
        job.Close = Close;
        return job;
    }

    /// <summary>Queues the like for the authenticated user. The Writer answers 201 for a new like, 200 if already liked, 404 if the post is missing.</summary>
    private int Like(long postId, Out o) {
        WriteJob job = NewJob(WriteJob.KindLike);
        job.PostId = postId;
        Writer.Submit(job);
        return Deferred;
    }

    /// <summary>Counts Unicode scalar values, as the SPEC 500-character limit does.</summary>
    private static int Characters(ReadOnlySpan<char> text) {
        int count = 0;
        for (int i = 0; i < text.Length; i++) {
            count++;
            if (char.IsHighSurrogate(text[i]) && i + 1 < text.Length && char.IsLowSurrogate(text[i + 1])) {
                i++;
            }
        }
        return count;
    }

    private enum BodyResult { Ok, Malformed, Required }

    /// <summary>
    /// Reads the document and copies the root object's "body" string into _chars. If "body"
    /// appears more than once, the last one wins. A malformed document is reported before a
    /// missing or non-string body, matching JsonDocument.Parse.
    /// </summary>
    private BodyResult ReadBody(ReadOnlySpan<byte> json, out int length) {
        length = -1;
        bool rootIsObject = false;
        try {
            Utf8JsonReader reader = new(json);
            while (reader.Read()) {
                if (reader.CurrentDepth == 0 && reader.TokenType == JsonTokenType.StartObject) {
                    rootIsObject = true;
                }

                if (reader.CurrentDepth != 1 || reader.TokenType != JsonTokenType.PropertyName
                    || !reader.ValueTextEquals("body"u8)) {
                    continue;
                }

                reader.Read();
                if (reader.TokenType != JsonTokenType.String) {
                    length = -1;
                    if (reader.TokenType is JsonTokenType.StartObject or JsonTokenType.StartArray) {
                        reader.Skip();
                    }

                    continue;
                }
                int needed = reader.HasValueSequence ? (int)reader.ValueSequence.Length : reader.ValueSpan.Length;
                if (needed > _chars.Length) {
                    _chars = new char[needed];
                }

                length = reader.CopyString(_chars);
            }
        } catch (Exception ex) when (ex is JsonException or InvalidOperationException) {
            return BodyResult.Malformed;
        }
        return rootIsObject && length >= 0 ? BodyResult.Ok : BodyResult.Required;
    }

    private enum IdResult { Ok, Invalid, TooLarge }

    /// <summary>
    /// A positive integer in plain decimal digits. Digits beyond SQLite's INTEGER range cannot
    /// name a row, so they answer 404 rather than 400 (as in the reference implementation).
    /// </summary>
    private static IdResult ParseId(ReadOnlySpan<byte> text, out long id) {
        id = 0;
        ulong value = 0;
        bool tooLarge = false;
        foreach (byte c in text) {
            if (c is < (byte)'0' or > (byte)'9') {
                return IdResult.Invalid;
            }

            if (tooLarge) {
                continue;
            }

            value = (value * 10) + (ulong)(c - '0');
            if (value > long.MaxValue) {
                tooLarge = true;
            }
        }
        if (tooLarge) {
            return IdResult.TooLarge;
        }

        if (value == 0) {
            return IdResult.Invalid;
        }

        id = (long)value;
        return IdResult.Ok;
    }

    /// <summary>Writes the 401 body for a failed authentication.</summary>
    private static int AuthError(Out o, AuthStatus status) {
        o.Bytes(status switch {
            AuthStatus.MissingToken => "{\"error\":\"missing bearer token\"}"u8,
            AuthStatus.BadPayload => "{\"error\":\"invalid token payload\"}"u8,
            _ => "{\"error\":\"invalid or expired token\"}"u8,
        });
        return 401;
    }

    private static ReadOnlySpan<byte> NotFound => "{\"error\":\"not found\"}"u8;
    private static ReadOnlySpan<byte> InvalidId => "{\"error\":\"invalid post id\"}"u8;
    private static ReadOnlySpan<byte> PostNotFound => "{\"error\":\"post not found\"}"u8;
    private static ReadOnlySpan<byte> Malformed => "{\"error\":\"malformed JSON body\"}"u8;
    private static ReadOnlySpan<byte> BodyRequired => "{\"error\":\"body is required\"}"u8;
    private static ReadOnlySpan<byte> BodyTooLong => "{\"error\":\"body must be at most 500 characters\"}"u8;
    private static ReadOnlySpan<byte> Internal => "{\"error\":\"internal server error\"}"u8;

    /// <summary>Writes an error body and returns its status.</summary>
    private static int Error(Out o, int status, ReadOnlySpan<byte> message) {
        o.Bytes(message);
        return status;
    }
}
