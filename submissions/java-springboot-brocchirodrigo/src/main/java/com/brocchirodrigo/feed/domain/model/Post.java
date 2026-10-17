package com.brocchirodrigo.feed.domain.model;

public record Post(long id, String body, String createdAt, String author, long likeCount) {
}
