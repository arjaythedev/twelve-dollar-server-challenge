package com.brocchirodrigo.feed.application.usecase.output;

public record PostNotFound() implements PostLookup {

    public static final PostNotFound INSTANCE = new PostNotFound();
}
