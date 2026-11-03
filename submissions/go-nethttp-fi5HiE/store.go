package main

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"maps"
	"net/url"
	"runtime"

	_ "github.com/mattn/go-sqlite3"
)

var errPostNotFound = errors.New("post not found")

type Post struct {
	ID        int64  `json:"id"`
	Body      string `json:"body"`
	CreatedAt string `json:"created_at"`
	Author    string `json:"author"`
	LikeCount int64  `json:"like_count"`
}

const selectPost = `
SELECT p.id, p.body, p.created_at, u.username AS author,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id`

const feedQuery = `
SELECT json_object('posts', json_group_array(
         json_object('id', id, 'body', body, 'created_at', created_at, 'author', author, 'like_count', like_count)))
  FROM (` + selectPost + ` ORDER BY p.created_at DESC, p.id DESC LIMIT 20)`

type Store struct {
	read  *sql.DB
	write *sql.DB

	feed       *sql.Stmt
	post       *sql.Stmt
	postExists *sql.Stmt
	createPost *sql.Stmt
	like       *sql.Stmt
}

func openStore(ctx context.Context, path string) (*Store, error) {
	params := url.Values{
		"_busy_timeout": {"5000"},
		"_journal_mode": {"WAL"},
		"_synchronous":  {"NORMAL"},
		"_mutex":        {"no"},
	}
	readParams := maps.Clone(params)
	readParams.Set("_query_only", "1")
	read, err := openPool(path, readParams, max(4, 2*runtime.GOMAXPROCS(0)))
	if err != nil {
		return nil, err
	}
	write, err := openPool(path, params, 1)
	if err != nil {
		return nil, errors.Join(err, read.Close())
	}
	s := &Store{read: read, write: write}

	stmts := []struct {
		db    *sql.DB
		dst   **sql.Stmt
		query string
	}{
		{read, &s.feed, feedQuery},
		{read, &s.post, selectPost + ` WHERE p.id = ?`},
		{read, &s.postExists, `SELECT 1 FROM posts WHERE id = ?`},
		{write, &s.createPost, `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`},
		{write, &s.like, `INSERT INTO likes (user_id, post_id)
		                  SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
		                  ON CONFLICT (user_id, post_id) DO NOTHING`},
	}
	for _, st := range stmts {
		if *st.dst, err = st.db.PrepareContext(ctx, st.query); err != nil {
			return nil, errors.Join(fmt.Errorf("prepare %q: %w", st.query, err), s.Close())
		}
	}
	return s, nil
}

func openPool(path string, params url.Values, conns int) (*sql.DB, error) {
	db, err := sql.Open("sqlite3", "file:"+path+"?"+params.Encode())
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	db.SetMaxOpenConns(conns)
	db.SetMaxIdleConns(conns)
	db.SetConnMaxLifetime(0)
	return db, nil
}

func (s *Store) Close() error {
	return errors.Join(s.read.Close(), s.write.Close())
}

func (s *Store) Ping() error {
	var one int
	return s.read.QueryRow(`SELECT 1`).Scan(&one)
}

func (s *Store) FeedJSON() (string, error) {
	var body string
	if err := s.feed.QueryRow().Scan(&body); err != nil {
		return "", fmt.Errorf("query feed: %w", err)
	}
	return body, nil
}

func (s *Store) Post(id int64) (Post, error) {
	var p Post
	err := s.post.QueryRow(id).Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount)
	if errors.Is(err, sql.ErrNoRows) {
		return Post{}, errPostNotFound
	}
	if err != nil {
		return Post{}, fmt.Errorf("query post %d: %w", id, err)
	}
	return p, nil
}

func (s *Store) CreatePost(userID int64, author, body string) (Post, error) {
	p := Post{Body: body, Author: author}
	if err := s.createPost.QueryRow(userID, body).Scan(&p.ID, &p.CreatedAt); err != nil {
		return Post{}, fmt.Errorf("insert post: %w", err)
	}
	return p, nil
}

func (s *Store) Like(userID, postID int64) (already bool, err error) {
	res, err := s.like.Exec(userID, postID)
	if err != nil {
		return false, fmt.Errorf("insert like: %w", err)
	}
	n, err := res.RowsAffected()
	if err != nil {
		return false, fmt.Errorf("insert like: %w", err)
	}
	if n == 1 {
		return false, nil
	}

	var one int
	err = s.postExists.QueryRow(postID).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return false, errPostNotFound
	}
	if err != nil {
		return false, fmt.Errorf("check post %d: %w", postID, err)
	}
	return true, nil
}
