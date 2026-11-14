package server.util;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

public class StringUtilTest {

    @Test
    void testTrimJsStandard() {
        assertEquals("hello", StringUtil.trimJs("  hello  "));
        assertEquals("hello", StringUtil.trimJs("\t\n\r hello\r\n\t "));
        assertEquals("", StringUtil.trimJs("   "));
        assertEquals("", StringUtil.trimJs(""));
        assertNull(StringUtil.trimJs(null));
    }

    @Test
    void testTrimJsUnicodeWhitespace() {
        // \u00A0 is non-breaking space
        assertEquals("", StringUtil.trimJs("\u00A0 \u00A0"));
        // \u3000 is ideographic space
        assertEquals("", StringUtil.trimJs("\u3000"));
        // \u2000 through \u200A
        assertEquals("content", StringUtil.trimJs("\u2002content\u2003"));
        assertEquals("test: <b>&amp;</b> \"quoted\" / café ✓",
                StringUtil.trimJs("  test: <b>&amp;</b> \"quoted\" / café ✓  "));
    }

    @Test
    void testParsePositiveId() {
        assertEquals(1L, StringUtil.parsePositiveId("1"));
        assertEquals(500000L, StringUtil.parsePositiveId("500000"));
        assertNull(StringUtil.parsePositiveId("0"));
        assertNull(StringUtil.parsePositiveId("-1"));
        assertNull(StringUtil.parsePositiveId("abc"));
        assertNull(StringUtil.parsePositiveId("1.5"));
        assertNull(StringUtil.parsePositiveId(""));
        assertNull(StringUtil.parsePositiveId(null));
    }
}
