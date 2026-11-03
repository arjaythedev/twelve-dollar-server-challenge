/* libFuzzer: the JSON reader must never crash or write past a field's buffer, whatever the document. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "json_read.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	enum { CAP = 16 };
	char body[CAP + 1], other[CAP + 1];

	/* One byte of guard after each buffer: the reader must not touch it. */
	body[CAP] = other[CAP] = 0x5a;
	json_field fields[] = { JSON_FIELD("body", body, CAP), JSON_FIELD("other", other, CAP), JSON_FIELD("n", NULL, 0) };

	json_read_object(strview_make((const char *)data, size), fields, 3);
	if (body[CAP] != 0x5a || other[CAP] != 0x5a)
		abort();
	for (int i = 0; i < 3; i++) {
		if (fields[i].text_len > fields[i].text_cap)
			abort();
	}
	return 0;
}
