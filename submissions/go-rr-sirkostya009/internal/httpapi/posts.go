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

// Get takes the id as a string so a bad one is a 400, not the 404 a typed
// path param would fall through to.
//
//rr:route GET /posts/{id}
func (a *FeedApi) Get(ctx context.Context, id string) (domain.PostEnvelope, error) {
	pid, err := domain.ParsePostID(id)
	if err != nil {
		return domain.PostEnvelope{}, err
	}
	p, err := a.Posts.Get(ctx, pid)
	return domain.PostEnvelope{Post: p}, err
}

// PostsApi is the writes. requireUser runs before the body is decoded and
// before the id is read, which is the spec's "auth first" order.
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

//rr:route POST /posts/{id}/like
func (a *PostsApi) Like(ctx context.Context, w http.ResponseWriter, id string) (domain.Like, error) {
	pid, err := domain.ParsePostID(id)
	if err != nil {
		return domain.Like{}, err
	}
	by, _ := domain.PrincipalFrom(ctx)
	l, err := a.Posts.Like(ctx, by, pid)
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
	w.Header()["Content-Type"] = jsonCT
	w.WriteHeader(http.StatusCreated)
}
