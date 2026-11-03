package io.github.wolches.challenge12.model.response;

import io.github.wolches.challenge12.model.PostDto;
import lombok.Builder;
import lombok.extern.jackson.Jacksonized;

import java.util.List;

@Jacksonized
@Builder
public record PostListResponseDto(
        List<PostDto> posts
) {
}
