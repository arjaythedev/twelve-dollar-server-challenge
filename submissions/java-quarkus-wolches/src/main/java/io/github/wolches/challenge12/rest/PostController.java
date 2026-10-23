package io.github.wolches.challenge12.rest;

import io.github.wolches.challenge12.model.PostCreateRequestDto;
import io.github.wolches.challenge12.model.PostId;
import io.github.wolches.challenge12.model.response.LikeResponseDto;
import io.github.wolches.challenge12.model.response.PostListResponseDto;
import io.github.wolches.challenge12.model.response.SinglePostResponseDto;
import io.github.wolches.challenge12.service.PostService;
import io.quarkus.security.Authenticated;
import io.smallrye.mutiny.Uni;
import jakarta.inject.Inject;
import jakarta.ws.rs.*;
import jakarta.ws.rs.core.MediaType;
import lombok.RequiredArgsConstructor;
import org.jboss.resteasy.reactive.RestResponse;

@Path("")
@RequiredArgsConstructor(onConstructor_ = @Inject)
public class PostController {

    private final PostService postService;

    @GET
    @Path("/posts/{id}")
    @Produces(MediaType.APPLICATION_JSON)
    public Uni<SinglePostResponseDto> getPostById(@PathParam("id") String id) {
        return postService
                .getPostById(PostId.fromString(id).value())
                .map(SinglePostResponseDto::new);
    }

    @POST
    @Path("/posts")
    @Authenticated
    @Produces(MediaType.APPLICATION_JSON)
    public Uni<RestResponse<SinglePostResponseDto>> addPost(PostCreateRequestDto postCreateRequestDto) {
        return postService
                .createPost(postCreateRequestDto)
                .map(SinglePostResponseDto::new)
                .map(response -> RestResponse.status(
                        RestResponse.Status.CREATED,
                        response));
    }

    @POST
    @Path("/posts/{id}/like")
    @Authenticated
    @Produces(MediaType.APPLICATION_JSON)
    public Uni<RestResponse<LikeResponseDto>> like(@PathParam("id") String id) {
        return postService
                .likePost(PostId.fromString(id).value())
                .map(like -> RestResponse.status(
                        like.alreadyLiked() ? RestResponse.Status.OK : RestResponse.Status.CREATED,
                        like));
    }

    @Path("/feed")
    @GET
    @Produces(MediaType.APPLICATION_JSON)
    public Uni<PostListResponseDto> feed() {
        return postService
                .getFeed()
                .map(PostListResponseDto::new);
    }
}
