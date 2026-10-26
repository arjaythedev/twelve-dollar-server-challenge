// Package service is the business layer between the http api and the data.
package service

import (
	"context"

	"feed/domain"
	"feed/internal/repository"
)

// PostService is everything the api does with posts.
type PostService interface {
	Feed(ctx context.Context) (domain.Feed, error)
	Get(ctx context.Context, id int64) (domain.Post, error)
	// Create writes in as by. in arrives validated and trimmed: CreatePost's
	// decode does that.
	Create(ctx context.Context, by domain.Principal, in domain.CreatePost) (domain.Post, error)
	Like(ctx context.Context, by domain.Principal, postID int64) (domain.Like, error)
}

// NewPostService builds a PostService over posts.
func NewPostService(posts repository.PostRepository) PostService {
	return &postService{posts}
}

type postService struct {
	posts repository.PostRepository
}

func (s *postService) Feed(ctx context.Context) (domain.Feed, error) {
	posts, err := s.posts.Feed(ctx)
	return domain.Feed{Posts: posts}, err
}

func (s *postService) Get(ctx context.Context, id int64) (domain.Post, error) {
	return s.posts.Get(ctx, id)
}

func (s *postService) Create(ctx context.Context, by domain.Principal, in domain.CreatePost) (domain.Post, error) {
	id, createdAt, err := s.posts.Insert(ctx, by.UserID, in.Body)
	if err != nil {
		return domain.Post{}, err
	}
	return domain.Post{ID: id, Body: in.Body, CreatedAt: createdAt, Author: by.Username}, nil
}

func (s *postService) Like(ctx context.Context, by domain.Principal, postID int64) (domain.Like, error) {
	inserted, err := s.posts.Like(ctx, by.UserID, postID)
	if err != nil {
		return domain.Like{}, err
	}
	return domain.Like{Liked: true, AlreadyLiked: !inserted, PostID: postID}, nil
}
