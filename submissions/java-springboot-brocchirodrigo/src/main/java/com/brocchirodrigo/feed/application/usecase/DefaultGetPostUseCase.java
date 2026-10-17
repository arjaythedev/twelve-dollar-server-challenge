package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.usecase.output.PostFound;
import com.brocchirodrigo.feed.application.usecase.output.PostLookup;
import com.brocchirodrigo.feed.application.usecase.output.PostNotFound;
import com.brocchirodrigo.feed.domain.repository.PostRepository;

public record DefaultGetPostUseCase(PostRepository posts) implements GetPostUseCase {

    @Override
    public PostLookup execute(long id) {
        return posts.findById(id).<PostLookup>map(PostFound::new).orElse(PostNotFound.INSTANCE);
    }
}
