package com.dglynch.twelvedollar;

public sealed interface LikeResult {
    record NewlyLiked() implements LikeResult {
    }

    record AlreadyLiked() implements LikeResult {
    }

    record PostNotFound() implements LikeResult {
    }
}
