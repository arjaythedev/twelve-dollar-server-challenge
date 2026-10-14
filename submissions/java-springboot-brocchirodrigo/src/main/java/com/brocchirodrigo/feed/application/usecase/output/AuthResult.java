package com.brocchirodrigo.feed.application.usecase.output;

public sealed interface AuthResult permits Authenticated, AuthRejected {
}
