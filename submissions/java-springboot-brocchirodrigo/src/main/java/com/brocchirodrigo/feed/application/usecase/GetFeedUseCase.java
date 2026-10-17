package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.domain.model.Post;

import java.util.List;

public interface GetFeedUseCase {

    List<Post> execute();
}
