#include "post_json.h"

void post_json_write(writer *w, const post_row *post)
{
	writer_literal(w, "{\"id\":");
	writer_u64(w, (uint64_t)post->id);
	writer_literal(w, ",\"body\":\"");
	writer_json_escaped(w, post->body);
	writer_literal(w, "\",\"created_at\":\"");
	writer_json_escaped(w, post->created_at);
	writer_literal(w, "\",\"author\":\"");
	writer_json_escaped(w, post->author);
	writer_literal(w, "\",\"like_count\":");
	writer_u64(w, (uint64_t)post->like_count);
	writer_literal(w, "}");
}
