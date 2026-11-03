package io.github.wolches.challenge12.model.persistent;

import lombok.Builder;

@Builder
public record PostResult(
    int id,
    String createdAt
) {
}
