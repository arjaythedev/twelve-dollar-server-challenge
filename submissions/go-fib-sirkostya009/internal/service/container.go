package service

import "feed/internal/repository"

// Container is every service there is, built on the data layer below it.
type Container struct {
	Posts PostService
}

// New wires the service layer over repos. It does no I/O.
func New(repos repository.Container) Container {
	return Container{Posts: NewPostService(repos.Posts)}
}
