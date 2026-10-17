package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.usecase.output.PostLookup;

public interface GetPostUseCase {

    PostLookup execute(long id);
}
