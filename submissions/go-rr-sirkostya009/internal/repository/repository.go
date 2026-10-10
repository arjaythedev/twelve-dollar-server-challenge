// Package repository is the data layer: the challenge's queries over SQLite.
package repository

import (
	"context"
	"database/sql"

	"feed/domain"
	"feed/internal/sqlite"
)

// PostRepository reads and writes posts and their likes.
type PostRepository interface {
	// Feed is the newest domain.FeedSize posts.
	Feed(ctx context.Context) ([]domain.Post, error)
	// Get is one post, or domain.ErrPostNotFound.
	Get(ctx context.Context, id int64) (domain.Post, error)
	// Insert writes a post and hands back its id and created_at.
	Insert(ctx context.Context, userID int64, body string) (int64, string, error)
	// Like records a like once per (user, post): inserted is false on a
	// repeat. A post that does not exist is domain.ErrPostNotFound.
	Like(ctx context.Context, userID, postID int64) (inserted bool, err error)
}

// Pinger is what the health check asks.
type Pinger interface {
	Ping(ctx context.Context) error
}

// Container is every repository there is, built on one database.
type Container struct {
	Posts PostRepository
	DB    Pinger
}

// New wires the data layer over db. It does no I/O.
func New(db *DB) Container {
	return Container{Posts: &postRepository{db}, DB: db}
}

const postColumns = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id`

const (
	sqlFeed   = postColumns + ` ORDER BY p.created_at DESC, p.id DESC LIMIT 20`
	sqlGet    = postColumns + ` WHERE p.id = ?`
	sqlPing   = `SELECT 1`
	sqlInsert = `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`
	// One statement: the like goes in only when the post exists, and a repeat
	// is no row rather than an error. Foreign keys are off in SQLite by
	// default, so the EXISTS is what keeps likes on missing posts out.
	sqlLike   = `INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING`
	sqlExists = `SELECT 1 FROM posts WHERE id = ?`
)

// readStmts are a reader connection's prepared statements.
type readStmts struct {
	feed, get, ping *sqlite.Stmt
}

type writeStmts struct {
	insert, like, exists *sqlite.Stmt
}

func prepareRead(c *sqlite.Conn) (readStmts, error) {
	var s readStmts
	var err error
	if s.feed, err = c.Prepare(sqlFeed); err != nil {
		return s, err
	}
	if s.get, err = c.Prepare(sqlGet); err != nil {
		return s, err
	}
	if s.ping, err = c.Prepare(sqlPing); err != nil {
		return s, err
	}
	return s, nil
}

func prepareWrite(c *sqlite.Conn) (writeStmts, error) {
	var s writeStmts
	var err error
	if s.insert, err = c.Prepare(sqlInsert); err != nil {
		return s, err
	}
	if s.like, err = c.Prepare(sqlLike); err != nil {
		return s, err
	}
	if s.exists, err = c.Prepare(sqlExists); err != nil {
		return s, err
	}
	return s, nil
}

type postRepository struct {
	db *DB
}

var _ PostRepository = (*postRepository)(nil)

// scanPost reads a row of postColumns.
func scanPost(s *sqlite.Stmt) domain.Post {
	return domain.Post{
		ID:        s.Int64(0),
		Body:      s.Text(1),
		CreatedAt: s.Text(2),
		Author:    s.Text(3),
		LikeCount: s.Int64(4),
	}
}

func (r *postRepository) Feed(ctx context.Context) ([]domain.Post, error) {
	rc, err := r.db.reader(ctx)
	if err != nil {
		return nil, err
	}
	defer r.db.release(rc)
	s := rc.stmts.feed
	defer s.Reset()
	posts := make([]domain.Post, 0, domain.FeedSize)
	for {
		row, err := s.Step()
		if err != nil {
			return nil, err
		}
		if !row {
			return posts, nil
		}
		posts = append(posts, scanPost(s))
	}
}

func (r *postRepository) Get(ctx context.Context, id int64) (domain.Post, error) {
	rc, err := r.db.reader(ctx)
	if err != nil {
		return domain.Post{}, err
	}
	defer r.db.release(rc)
	s := rc.stmts.get
	defer s.Reset()
	if err := s.BindInt64(1, id); err != nil {
		return domain.Post{}, err
	}
	row, err := s.Step()
	switch {
	case err != nil:
		return domain.Post{}, err
	case !row:
		return domain.Post{}, domain.ErrPostNotFound
	}
	return scanPost(s), nil
}

func (r *postRepository) Insert(ctx context.Context, userID int64, body string) (int64, string, error) {
	w, err := r.db.writer(ctx)
	if err != nil {
		return 0, "", err
	}
	defer r.db.releaseWriter()
	s := w.insert
	defer s.Reset()
	if err := s.BindInt64(1, userID); err != nil {
		return 0, "", err
	}
	if err := s.BindText(2, body); err != nil {
		return 0, "", err
	}
	row, err := s.Step()
	if err != nil {
		return 0, "", err
	}
	if !row {
		return 0, "", sql.ErrNoRows
	}
	id, createdAt := s.Int64(0), s.Text(1)
	// stepping to done ends the statement, which commits it (autocommit)
	if _, err := s.Step(); err != nil {
		return 0, "", err
	}
	return id, createdAt, nil
}

func (r *postRepository) Like(ctx context.Context, userID, postID int64) (bool, error) {
	w, err := r.db.writer(ctx)
	if err != nil {
		return false, err
	}
	defer r.db.releaseWriter()
	s := w.like
	defer s.Reset()
	if err := s.BindInt64(1, userID); err != nil {
		return false, err
	}
	if err := s.BindInt64(2, postID); err != nil {
		return false, err
	}
	if _, err := s.Step(); err != nil {
		return false, err
	}
	if r.db.w.Changes() == 1 {
		return true, nil
	}
	// nothing went in: a repeat, or no such post
	e := w.exists
	defer e.Reset()
	if err := e.BindInt64(1, postID); err != nil {
		return false, err
	}
	row, err := e.Step()
	switch {
	case err != nil:
		return false, err
	case !row:
		return false, domain.ErrPostNotFound
	}
	return false, nil
}
