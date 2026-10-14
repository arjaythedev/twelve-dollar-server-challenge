package com.brocchirodrigo.feed.adapter.presenter.dto;

public record LikeResponse(boolean liked, boolean alreadyLiked, long postId) {

    public static LikeResponse liked(long postId) {
        return new LikeResponse(true, false, postId);
    }

    public static LikeResponse alreadyLiked(long postId) {
        return new LikeResponse(true, true, postId);
    }
}
