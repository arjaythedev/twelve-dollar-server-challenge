package httpapi

import (
	"context"
	"net/http"

	"feed/domain"
	"feed/internal/service"
)

// FeedApi is the public reads.
type FeedApi struct {
	Posts service.PostService
}

//rr:route GET /feed
func (a *FeedApi) Feed(ctx context.Context) (domain.Feed, error) {
	return a.Posts.Feed(ctx)
}

//rr:route GET /posts/{id=@domain.ParsePostID}
func (a *FeedApi) Get(ctx context.Context, id int64) (domain.PostEnvelope, error) {
	p, err := a.Posts.Get(ctx, id)
	return domain.PostEnvelope{Post: p}, err
}

// PostsApi is the writes. requireUser runs before the body is decoded, which
// is the spec's "auth first" order. A bad id fails routing first and gets the
// same check in notFound.
//
//rr:pre @requireUser
type PostsApi struct {
	Posts service.PostService
}

//rr:route POST /posts
func (a *PostsApi) Create(ctx context.Context, w http.ResponseWriter, body domain.CreatePost) (domain.PostEnvelope, error) {
	by, _ := domain.PrincipalFrom(ctx) // requireUser let the request through
	p, err := a.Posts.Create(ctx, by, body)
	if err != nil {
		return domain.PostEnvelope{}, err
	}
	created(w)
	return domain.PostEnvelope{Post: p}, nil
}

//rr:route POST /posts/{id=@domain.ParsePostID}/like
func (a *PostsApi) Like(ctx context.Context, w http.ResponseWriter, id int64) (domain.Like, error) {
	by, _ := domain.PrincipalFrom(ctx)
	l, err := a.Posts.Like(ctx, by, id)
	if err != nil {
		return domain.Like{}, err
	}
	if !l.AlreadyLiked {
		created(w)
	}
	return l, nil
}

// created sends a 201. The generated code sets Content-Type after the handler
// returns, too late once the status is out, so it is set here first.
func created(w http.ResponseWriter) {
	w.Header()["Content-Type"] = __jsonCT
	w.WriteHeader(http.StatusCreated)
}
