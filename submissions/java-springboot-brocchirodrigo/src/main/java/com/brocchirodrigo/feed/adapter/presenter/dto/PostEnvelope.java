package com.brocchirodrigo.feed.adapter.presenter.dto;

import com.brocchirodrigo.feed.domain.model.Post;

public record PostEnvelope(PostResponse post) {

    public static PostEnvelope from(Post post) {
        return new PostEnvelope(PostResponse.from(post));
    }
}
