package main

import "encoding/json"

// Post is the wire representation of a post. Field order is significant: the
// spec requires id, body, created_at, author, like_count in exactly this order.
type Post struct {
	ID        int64  `json:"id"`
	Body      string `json:"body"`
	CreatedAt string `json:"created_at"`
	Author    string `json:"author"`
	LikeCount int64  `json:"like_count"`
}

type postEnvelope struct {
	Post Post `json:"post"`
}

type feedEnvelope struct {
	Posts []Post `json:"posts"`
}

type likeResponse struct {
	Liked        bool  `json:"liked"`
	AlreadyLiked bool  `json:"already_liked"`
	PostID       int64 `json:"post_id"`
}

type healthOK struct {
	Status  string `json:"status"`
	DB      string `json:"db"`
	UptimeS int64  `json:"uptime_s"`
}

type healthDegraded struct {
	Status string `json:"status"`
	DB     string `json:"db"`
	Error  string `json:"error"`
}

type errorResponse struct {
	Error string `json:"error"`
}

// claims is the subset of the JWT payload the API needs.
type claims struct {
	Sub      json.RawMessage `json:"sub"`
	Username json.RawMessage `json:"username"`
	Exp      float64         `json:"exp"`
}
