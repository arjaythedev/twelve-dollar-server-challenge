#include "check.h"
#include "json_read.h"

static json_document read_body(const char *json, char *buffer, size_t cap, json_field *field)
{
	*field = (json_field)JSON_FIELD("body", buffer, cap);
	return json_read_object(strview_make(json, strlen(json)), field, 1);
}

int main(void)
{
	char text[64];
	json_field f;

	/* a plain string */
	CHECK(read_body("{\"body\":\"hi\"}", text, sizeof text, &f) == JSON_DOC_OBJECT);
	CHECK(f.kind == JSON_STRING && f.text_len == 2 && memcmp(text, "hi", 2) == 0);

	/* whitespace, other members, and the last duplicate wins */
	CHECK(read_body(" { \"a\" : [1, {\"x\": null}] , \"body\" : \"one\" , \"body\":\"two\" } ", text, sizeof text, &f) == JSON_DOC_OBJECT);
	CHECK(f.kind == JSON_STRING && f.text_len == 3 && memcmp(text, "two", 3) == 0);

	/* escapes */
	CHECK(read_body("{\"body\":\"q\\\"b\\\\s\\/n\\n\\u00e9\\u2713\"}", text, sizeof text, &f) == JSON_DOC_OBJECT);
	CHECK(f.text_len == 13 && memcmp(text, "q\"b\\s/n\n\xC3\xA9\xE2\x9C\x93", 13) == 0);
	CHECK(read_body("{\"body\":\"\\ud83d\\ude00\"}", text, sizeof text, &f) == JSON_DOC_OBJECT);
	CHECK(f.text_len == 4 && memcmp(text, "\xF0\x9F\x98\x80", 4) == 0);   /* surrogate pair */
	CHECK(read_body("{\"body\":\"\\ud83dx\"}", text, sizeof text, &f) == JSON_DOC_OBJECT);
	CHECK(f.text_len == 4 && memcmp(text, "\xEF\xBF\xBD" "x", 4) == 0);   /* lone surrogate -> U+FFFD */

	/* kinds */
	CHECK(read_body("{\"body\":123}", text, sizeof text, &f) == JSON_DOC_OBJECT && f.kind == JSON_NUMBER && f.number == 123);
	CHECK(read_body("{\"body\":-1.5e2}", text, sizeof text, &f) == JSON_DOC_OBJECT && f.kind == JSON_NUMBER && f.number == -150);
	CHECK(read_body("{\"body\":null}", text, sizeof text, &f) == JSON_DOC_OBJECT && f.kind == JSON_OTHER);
	CHECK(read_body("{\"body\":[1]}", text, sizeof text, &f) == JSON_DOC_OBJECT && f.kind == JSON_OTHER);
	CHECK(read_body("{}", text, sizeof text, &f) == JSON_DOC_OBJECT && f.kind == JSON_ABSENT);

	/* a string that does not fit is flagged; the document is still valid */
	CHECK(read_body("{\"body\":\"0123456789\"}", text, 4, &f) == JSON_DOC_OBJECT && f.kind == JSON_STRING && f.truncated);

	/* valid JSON that is not an object */
	CHECK(read_body("[1,2]", text, sizeof text, &f) == JSON_DOC_NOT_OBJECT);
	CHECK(read_body("42", text, sizeof text, &f) == JSON_DOC_NOT_OBJECT);
	CHECK(read_body("\"s\"", text, sizeof text, &f) == JSON_DOC_NOT_OBJECT);

	/* malformed documents */
	const char *bad[] = {
		"", "{", "{bad json", "{\"body\":}", "{\"body\":\"x\"", "{\"body\":\"x\",}", "{\"body\" \"x\"}",
		"{\"body\":'x'}", "{\"body\":\"a\nb\"}", "{\"body\":\"\\x\"}", "{\"body\":\"\\u12\"}", "{\"body\":01}",
		"{\"body\":1.}", "{\"body\":tru}", "{\"body\":\"x\"} trailing", "{} {}", "[1,]", "nul",
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		if (read_body(bad[i], text, sizeof text, &f) != JSON_DOC_INVALID)
			fprintf(stderr, "accepted malformed JSON: %s\n", bad[i]), check_failures++;
	}

	/* nesting depth is bounded */
	{
		char document[128];

		for (int depth = 30; depth <= 40; depth += 10) {
			int len = sprintf(document, "{\"x\":");
			memset(document + len, '[', (size_t)depth);
			memset(document + len + depth, ']', (size_t)depth);
			strcpy(document + len + 2 * depth, "}");
			json_document expected = depth <= 30 ? JSON_DOC_OBJECT : JSON_DOC_INVALID;
			CHECK(read_body(document, text, sizeof text, &f) == expected);
		}
	}
	TEST_MAIN_END();
}
