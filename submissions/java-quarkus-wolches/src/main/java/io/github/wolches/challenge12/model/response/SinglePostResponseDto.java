package io.github.wolches.challenge12.model.response;

import io.github.wolches.challenge12.model.PostDto;
import lombok.Builder;
import lombok.extern.jackson.Jacksonized;

@Jacksonized
@Builder
public record SinglePostResponseDto(
        PostDto post
) {
}
