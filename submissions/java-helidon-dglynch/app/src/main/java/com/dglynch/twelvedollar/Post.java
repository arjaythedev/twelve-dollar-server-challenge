package com.dglynch.twelvedollar;

import io.helidon.json.binding.Json;

@Json.Entity
public record Post(
        long id,
        String body,
        @Json.Property("created_at") String createdAt,
        String author,
        @Json.Property("like_count") long likeCount
) {
}
