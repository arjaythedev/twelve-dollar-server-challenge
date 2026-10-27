package challenge.feed.post

import challenge.feed.error.BadRequest
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive

/**
 * The `body` field of `POST /posts`: a JSON string, non-empty after trimming, at most 500 characters.
 * Characters are counted as code points, like the schema's `CHECK (length(body) ...)`. A NUL makes SQLite's
 * `length()` stop counting, so the database would store or reject such a body inconsistently: it is refused here.
 */
object PostBody {
    const val MAX_LENGTH = 500

    fun parse(raw: JsonElement?): String {
        val text = (raw as? JsonPrimitive)?.takeIf { it.isString }?.content?.trim()
        if (text.isNullOrEmpty() || '\u0000' in text) throw BadRequest("body is required")
        if (text.codePointCount(0, text.length) > MAX_LENGTH) throw BadRequest("body must be at most $MAX_LENGTH characters")
        return text
    }
}
