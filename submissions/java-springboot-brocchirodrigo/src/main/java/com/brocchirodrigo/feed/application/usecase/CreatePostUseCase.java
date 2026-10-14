package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.usecase.input.CreatePostCommand;
import com.brocchirodrigo.feed.application.usecase.output.CreatePostResult;

public interface CreatePostUseCase {

    CreatePostResult execute(CreatePostCommand command);
}
