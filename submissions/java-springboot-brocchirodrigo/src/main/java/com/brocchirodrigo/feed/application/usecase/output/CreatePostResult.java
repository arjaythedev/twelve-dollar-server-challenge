package com.brocchirodrigo.feed.application.usecase.output;

public sealed interface CreatePostResult permits PostCreated, PostRejected {
}
