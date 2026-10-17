package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.usecase.input.LikePostCommand;
import com.brocchirodrigo.feed.domain.model.enums.LikeOutcome;
import com.brocchirodrigo.feed.domain.repository.PostRepository;

public record DefaultLikePostUseCase(PostRepository posts) implements LikePostUseCase {

    @Override
    public LikeOutcome execute(LikePostCommand command) {
        if (posts.addLike(command.userId(), command.postId())) {
            return LikeOutcome.LIKED;
        }
        return posts.exists(command.postId()) ? LikeOutcome.ALREADY_LIKED : LikeOutcome.POST_NOT_FOUND;
    }
}
