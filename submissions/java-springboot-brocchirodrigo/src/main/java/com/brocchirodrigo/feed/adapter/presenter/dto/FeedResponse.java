package com.brocchirodrigo.feed.adapter.presenter.dto;

import com.brocchirodrigo.feed.domain.model.Post;

import java.util.List;

public record FeedResponse(List<PostResponse> posts) {

    public static FeedResponse from(List<Post> posts) {
        return new FeedResponse(posts.stream().map(PostResponse::from).toList());
    }
}
