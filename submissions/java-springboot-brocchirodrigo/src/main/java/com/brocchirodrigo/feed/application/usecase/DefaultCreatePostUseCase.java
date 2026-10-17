package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.usecase.input.CreatePostCommand;
import com.brocchirodrigo.feed.application.usecase.output.CreatePostResult;
import com.brocchirodrigo.feed.application.usecase.output.PostCreated;
import com.brocchirodrigo.feed.application.usecase.output.PostRejected;
import com.brocchirodrigo.feed.domain.model.Post;
import com.brocchirodrigo.feed.domain.model.PostBody;
import com.brocchirodrigo.feed.domain.model.PostBodyRejected;
import com.brocchirodrigo.feed.domain.repository.PostRepository;

public record DefaultCreatePostUseCase(PostRepository posts) implements CreatePostUseCase {

    @Override
    public CreatePostResult execute(CreatePostCommand command) {
        return switch (PostBody.of(command.body())) {
            case PostBody(var body) -> {
                var persisted = posts.create(command.authorId(), body);
                yield new PostCreated(new Post(persisted.id(), body, persisted.createdAt(), command.authorName(), 0));
            }
            case PostBodyRejected(var violation) -> new PostRejected(violation);
        };
    }
}
