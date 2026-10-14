package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.usecase.input.LikePostCommand;
import com.brocchirodrigo.feed.domain.model.enums.LikeOutcome;

public interface LikePostUseCase {

    LikeOutcome execute(LikePostCommand command);
}
