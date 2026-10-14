package com.brocchirodrigo.feed.adapter.controller.dto;

import tools.jackson.databind.JsonNode;
import tools.jackson.databind.node.ObjectNode;
import tools.jackson.databind.node.StringNode;

import java.util.Optional;

public record CreatePostRequest(String body) {

    public static Optional<CreatePostRequest> from(JsonNode node) {
        return switch (node) {
            case ObjectNode object when object.get("body") instanceof StringNode body ->
                    Optional.of(new CreatePostRequest(body.stringValue()));
            case null, default -> Optional.empty();
        };
    }
}
