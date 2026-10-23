package io.github.wolches.challenge12.service;

import io.github.wolches.challenge12.dao.AsyncSeparatedReadWriteDao;
import io.github.wolches.challenge12.exception.InvalidOrExpiredTokenException;
import io.github.wolches.challenge12.model.PostCreateRequestDto;
import io.github.wolches.challenge12.model.PostDto;
import io.github.wolches.challenge12.model.response.LikeResponseDto;
import io.smallrye.mutiny.Uni;
import jakarta.enterprise.context.ApplicationScoped;
import jakarta.inject.Inject;
import lombok.RequiredArgsConstructor;
import org.eclipse.microprofile.jwt.JsonWebToken;
import org.jspecify.annotations.NonNull;

import java.util.List;

@ApplicationScoped
@RequiredArgsConstructor(onConstructor_ = @Inject)
public class PostService {

    private final JsonWebToken jwt;
    private final BodyService bodyService;
    private final AsyncSeparatedReadWriteDao asyncSeparatedReadWriteDao;

    public Uni<PostDto> createPost(PostCreateRequestDto request) {
        var body = bodyService.getBodyString(request);
        var user = getUsername();
        return asyncSeparatedReadWriteDao
                .post(getUserId(), body)
                .map(result -> PostDto.builder()
                        .id(result.id())
                        .createdAt(result.createdAt())
                        .likeCount(0)
                        .author(user)
                        .body(body)
                        .build());
    }

    public Uni<LikeResponseDto> likePost(int id) {
        return asyncSeparatedReadWriteDao
                .like(getUserId(), id)
                .map(liked -> LikeResponseDto.builder()
                        .postId(id)
                        .liked(true)
                        .alreadyLiked(!liked)
                        .build());
    }

    public Uni<PostDto> getPostById(int id) {
        return asyncSeparatedReadWriteDao.getPostById(id);
    }

    public Uni<List<PostDto>> getFeed() {
        return asyncSeparatedReadWriteDao.getFeed();
    }

    private @NonNull String getUsername() {
        return jwt
                .claim("username")
                .map(String::valueOf)
                .orElseThrow(InvalidOrExpiredTokenException::new);
    }

    private int getUserId() {
        return jwt
                .claim("sub")
                .map(sub -> Integer.parseInt(sub.toString()))
                .orElseThrow(InvalidOrExpiredTokenException::new);
    }
}
