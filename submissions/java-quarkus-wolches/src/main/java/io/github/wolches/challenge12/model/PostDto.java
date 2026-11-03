package io.github.wolches.challenge12.model;

import com.fasterxml.jackson.annotation.JsonProperty;
import com.fasterxml.jackson.annotation.JsonPropertyOrder;
import lombok.Builder;
import lombok.extern.jackson.Jacksonized;

@Jacksonized
@Builder
@JsonPropertyOrder({"id", "body", "created_at", "author", "like_count"})
public record PostDto(
        int id,
        String body,
        @JsonProperty("created_at") String createdAt,
        String author,
        @JsonProperty("like_count") int likeCount
) {
}
