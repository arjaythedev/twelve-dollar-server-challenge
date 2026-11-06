package server.util;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

public class BodyParserTest {

    @Test
    void testValidBody() {
        BodyParser.ParseResult res = BodyParser.parsePostBody("{\"body\":\"hello world\"}");
        assertInstanceOf(BodyParser.ParseResult.Success.class, res);
        assertEquals("hello world", ((BodyParser.ParseResult.Success) res).body());
    }

    @Test
    void testBodyWithSpecialCharacters() {
        String input = "{\"body\":\"  test: <b>&amp;</b> \\\"quoted\\\" / café ✓  \"}";
        BodyParser.ParseResult res = BodyParser.parsePostBody(input);
        assertInstanceOf(BodyParser.ParseResult.Success.class, res);
        assertEquals("test: <b>&amp;</b> \"quoted\" / café ✓", ((BodyParser.ParseResult.Success) res).body());
    }

    @Test
    void testEmptyAndWhitespaceBody() {
        assertInstanceOf(BodyParser.ParseResult.BodyRequired.class, BodyParser.parsePostBody("{\"body\":\"\"}"));
        assertInstanceOf(BodyParser.ParseResult.BodyRequired.class, BodyParser.parsePostBody("{\"body\":\"   \"}"));
        assertInstanceOf(BodyParser.ParseResult.BodyRequired.class, BodyParser.parsePostBody("{\"body\":\"\t\n  \"}"));
    }

    @Test
    void testNumericOrMissingBody() {
        assertInstanceOf(BodyParser.ParseResult.BodyRequired.class, BodyParser.parsePostBody("{\"body\":123}"));
        assertInstanceOf(BodyParser.ParseResult.BodyRequired.class, BodyParser.parsePostBody("{}"));
    }

    @Test
    void testMalformedJson() {
        assertInstanceOf(BodyParser.ParseResult.MalformedJson.class, BodyParser.parsePostBody("{bad json"));
        assertInstanceOf(BodyParser.ParseResult.MalformedJson.class, BodyParser.parsePostBody("not json"));
        assertInstanceOf(BodyParser.ParseResult.MalformedJson.class, BodyParser.parsePostBody(null));
    }

    @Test
    void testLength500And501() {
        String body500 = "a".repeat(500);
        BodyParser.ParseResult res500 = BodyParser.parsePostBody("{\"body\":\"  " + body500 + "  \"}");
        assertInstanceOf(BodyParser.ParseResult.Success.class, res500);
        assertEquals(500, ((BodyParser.ParseResult.Success) res500).body().length());

        String body501 = "a".repeat(501);
        BodyParser.ParseResult res501 = BodyParser.parsePostBody("{\"body\":\"" + body501 + "\"}");
        assertInstanceOf(BodyParser.ParseResult.BodyTooLong.class, res501);
    }
}
