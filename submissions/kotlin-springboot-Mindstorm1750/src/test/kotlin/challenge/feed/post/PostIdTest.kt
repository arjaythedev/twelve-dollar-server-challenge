package challenge.feed.post

import challenge.feed.error.BadRequest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith

class PostIdTest {
    @Test
    fun `parses positive ids`() {
        assertEquals(1L, PostId.parse("1"))
        assertEquals(250000L, PostId.parse("250000"))
    }

    @Test
    fun `rejects ids the spec calls invalid`() {
        for (raw in listOf("0", "-1", "abc", "1.5")) {
            assertEquals("invalid post id", assertFailsWith<BadRequest> { PostId.parse(raw) }.message, "id '$raw'")
        }
    }
}
