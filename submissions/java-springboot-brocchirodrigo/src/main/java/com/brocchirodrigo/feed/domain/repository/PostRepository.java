package com.brocchirodrigo.feed.domain.repository;

import com.brocchirodrigo.feed.domain.model.PersistedPost;
import com.brocchirodrigo.feed.domain.model.Post;

import java.util.List;
import java.util.Optional;

public interface PostRepository {

    List<Post> latest(int limit);

    Optional<Post> findById(long id);

    PersistedPost create(long authorId, String body);

    boolean addLike(long userId, long postId);

    boolean exists(long id);
}
