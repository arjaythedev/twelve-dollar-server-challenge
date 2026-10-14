package com.brocchirodrigo.feed.adapter.presenter.dto;

import com.brocchirodrigo.feed.domain.model.Post;

public record PostResponse(long id, String body, String createdAt, String author, long likeCount) {

    public static PostResponse from(Post post) {
        return new PostResponse(post.id(), post.body(), post.createdAt(), post.author(), post.likeCount());
    }
}
