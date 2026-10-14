package com.brocchirodrigo.feed.application.usecase.input;

public record CreatePostCommand(long authorId, String authorName, String body) {
}
