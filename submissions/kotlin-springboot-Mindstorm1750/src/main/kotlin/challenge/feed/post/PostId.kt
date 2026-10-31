package challenge.feed.post

import challenge.feed.error.BadRequest
import challenge.feed.toPositiveLongOrNull

/** The `:id` path segment: a positive integer, else 400 `invalid post id`. */
object PostId {
    fun parse(raw: String): Long = raw.toPositiveLongOrNull() ?: throw BadRequest("invalid post id")
}
