package com.brocchirodrigo.feed.application.usecase.output;

import com.brocchirodrigo.feed.domain.model.Post;

public record PostCreated(Post post) implements CreatePostResult {
}
