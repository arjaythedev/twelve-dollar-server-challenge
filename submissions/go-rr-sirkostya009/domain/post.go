package domain

//go:generate go tool ggen -simd avx2 .

// Post is a post as the api answers with it. Keys go out in declaration
// order, which the spec fixes.
//
//ggen:generate nosortkeys
type Post struct {
	ID        int64  `json:"id"`
	Body      string `json:"body"`
	CreatedAt string `json:"created_at"`
	Author    string `json:"author"`
	LikeCount int64  `json:"like_count"`
}

//ggen:generate nosortkeys
type PostEnvelope struct {
	Post Post `json:"post"`
}

//ggen:generate nosortkeys
type Feed struct {
	Posts []Post `json:"posts"`
}

// FeedSize is how many posts the feed holds.
const FeedSize = 20

// MaxBody is the most characters a post body may hold, after trimming. The
// schema's CHECK counts characters too, so this is runes, not bytes.
const MaxBody = 500

// CreatePost is the body of POST /posts.
//
//ggen:generate ignoreunknown allowdups
type CreatePost struct {
	Body string `json:"body" pipe:"required trim notempty maxrunes=500"`
}

//ggen:generate nosortkeys
type Like struct {
	Liked        bool  `json:"liked"`
	AlreadyLiked bool  `json:"already_liked"`
	PostID       int64 `json:"post_id"`
}

// ParsePostID reads a path id: a positive integer in plain decimal digits.
func ParsePostID(s string) (int64, error) {
	if s == "" || len(s) > 18 { // 18 digits never overflow int64
		return 0, ErrInvalidPostID
	}
	var n int64
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < '0' || c > '9' {
			return 0, ErrInvalidPostID
		}
		n = n*10 + int64(c-'0')
	}
	if n == 0 {
		return 0, ErrInvalidPostID
	}
	return n, nil
}
