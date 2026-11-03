package io.github.wolches.challenge12.model;

import com.fasterxml.jackson.databind.JsonNode;
import lombok.Builder;
import lombok.extern.jackson.Jacksonized;

@Jacksonized
@Builder
public record PostCreateRequestDto(
        JsonNode body
) {
}
