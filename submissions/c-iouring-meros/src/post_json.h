/* The JSON shape of a post, shared by every endpoint that returns one. */
#ifndef POST_JSON_H
#define POST_JSON_H

#include "store.h"
#include "writer.h"

/* {"id":..,"body":..,"created_at":..,"author":..,"like_count":..} with the keys in exactly this order. */
void post_json_write(writer *w, const post_row *post);

#endif
