package io.github.wolches.challenge12.model.response;

import com.fasterxml.jackson.annotation.JsonProperty;
import com.fasterxml.jackson.annotation.JsonPropertyOrder;
import lombok.Builder;
import lombok.extern.jackson.Jacksonized;

@Jacksonized
@Builder
@JsonPropertyOrder({"liked", "already_liked", "post_id"})
public record LikeResponseDto(
        boolean liked,
        @JsonProperty("already_liked") boolean alreadyLiked,
        @JsonProperty("post_id") int postId
) {
}
