package challenge.feed.post

import challenge.feed.error.BadRequest
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class PostBodyTest {
    private fun rejected(raw: JsonElement?) = assertFailsWith<BadRequest> { PostBody.parse(raw) }.message

    @Test
    fun `trims and returns string bodies`() {
        assertEquals("hello", PostBody.parse(JsonPrimitive("  hello \n")))
        assertEquals("""a "quoted" / café ✓""", PostBody.parse(JsonPrimitive(""" a "quoted" / café ✓ """)))
    }

    @Test
    fun `missing, empty, blank or non-string bodies are required errors`() {
        for (raw in listOf(null, JsonNull, JsonPrimitive(""), JsonPrimitive("   "), JsonPrimitive(123), JsonPrimitive(true), JsonObject(emptyMap()))) {
            assertEquals("body is required", rejected(raw), "body $raw")
        }
    }

    @Test
    fun `length is checked after trimming`() {
        assertEquals("b".repeat(500), PostBody.parse(JsonPrimitive("  " + "b".repeat(500) + " ")))
        assertEquals("body must be at most 500 characters", rejected(JsonPrimitive("a".repeat(501))))
    }

    @Test
    fun `length counts characters, not UTF-16 units, like SQLite's length()`() {
        val emoji = "😀" // one character, two UTF-16 units
        assertEquals(emoji.repeat(500), PostBody.parse(JsonPrimitive(emoji.repeat(500))))
        assertEquals("body must be at most 500 characters", rejected(JsonPrimitive(emoji.repeat(501))))
    }

    @Test
    fun `bodies containing NUL are rejected, since SQLite would store or count them wrongly`() {
        assertEquals("body is required", rejected(JsonPrimitive("\u0000abc")))
        assertEquals("body is required", rejected(JsonPrimitive("abc\u0000def")))
    }

    @Test
    fun `500 non-ascii characters are accepted`() {
        val body = "é".repeat(500) // 1000 UTF-8 bytes, 500 characters
        assertEquals(body, PostBody.parse(JsonPrimitive(body)))
    }
}
