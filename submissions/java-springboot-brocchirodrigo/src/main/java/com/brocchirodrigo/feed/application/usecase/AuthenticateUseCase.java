package com.brocchirodrigo.feed.application.usecase;

import com.brocchirodrigo.feed.application.usecase.output.AuthResult;

public interface AuthenticateUseCase {

    AuthResult execute(String authorization);
}
