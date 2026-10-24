package challenge.feed

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

class PositiveIdsTest {
    @Test
    fun `accepts positive integers`() {
        assertEquals(1L, "1".toPositiveLongOrNull())
        assertEquals(500000L, "500000".toPositiveLongOrNull())
        assertEquals(Long.MAX_VALUE, Long.MAX_VALUE.toString().toPositiveLongOrNull())
    }

    @Test
    fun `rejects everything else`() {
        for (raw in listOf("0", "-1", "abc", "1.5", "", " 1", "1 ", "01", "+1", "1e3", "0x10", "9223372036854775808")) {
            assertNull(raw.toPositiveLongOrNull(), "expected null for '$raw'")
        }
    }
}
