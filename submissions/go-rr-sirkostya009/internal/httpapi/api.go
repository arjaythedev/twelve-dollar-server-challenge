package httpapi

//go:generate go tool rr $GOFILE

//rr:api onerror=@handleError on400=@badRequest on404=@notFound on405=@methodNotAllowed
type Api struct {
	HealthApi
	FeedApi
	PostsApi
}
