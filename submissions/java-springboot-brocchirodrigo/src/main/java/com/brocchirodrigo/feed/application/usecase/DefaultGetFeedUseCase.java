package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.domain.model.Post;
import com.brocchirodrigo.feed.domain.repository.PostRepository;

import java.util.List;

public record DefaultGetFeedUseCase(PostRepository posts) implements GetFeedUseCase {

    private static final int FEED_SIZE = 20;

    @Override
    public List<Post> execute() {
        return posts.latest(FEED_SIZE);
    }
}
