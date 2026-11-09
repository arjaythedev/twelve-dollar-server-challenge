package main

import (
	"database/sql"
	"net/url"
	"path/filepath"

	_ "github.com/mattn/go-sqlite3"
)

type store struct{ db *sql.DB }

func openStore(path string) (*store, error) {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	uri := url.URL{Scheme: "file", Path: absolute}
	query := url.Values{"mode": {"rw"}, "_journal_mode": {"WAL"}, "_synchronous": {"NORMAL"}, "_foreign_keys": {"on"}, "_busy_timeout": {"5000"}}
	uri.RawQuery = query.Encode()
	db, err := sql.Open("sqlite3", uri.String())
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	s := &store{db}
	if err := s.Health(); err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}
func (s *store) Close() error { return s.db.Close() }

func (s *store) Health() error {
	var one int
	return s.db.QueryRow("SELECT 1").Scan(&one)
}

const selectPost = `SELECT p.id, p.body, p.created_at, u.username,
 (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
 FROM posts p JOIN users u ON u.id = p.user_id`

func (s *store) Feed() ([]post, error) {
	rows, err := s.db.Query(selectPost + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	posts := make([]post, 0, 20)
	for rows.Next() {
		var p post
		if err := rows.Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount); err != nil {
			return nil, err
		}
		posts = append(posts, p)
	}
	return posts, rows.Err()
}

func (s *store) Post(id int64) (post, error) {
	var p post
	err := s.db.QueryRow(selectPost+" WHERE p.id = ?", id).Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount)
	return p, err
}

func (s *store) CreatePost(actor identity, body postBody) (post, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return post{}, err
	}
	defer tx.Rollback()
	p := post{Body: string(body), Author: actor.username}
	rows, err := tx.Query("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at", actor.userID, string(body))
	if err != nil {
		return post{}, err
	}
	if !rows.Next() {
		err = rows.Err()
		rows.Close()
		if err == nil {
			err = sql.ErrNoRows
		}
		return post{}, err
	}
	if err := rows.Scan(&p.ID, &p.CreatedAt); err != nil {
		rows.Close()
		return post{}, err
	}
	if err := rows.Close(); err != nil {
		return post{}, err
	}
	if err := tx.Commit(); err != nil {
		return post{}, err
	}
	return p, nil
}

func (s *store) Like(actor identity, postID int64) (bool, error) {
	tx, err := s.db.Begin()
	if err != nil {
		return false, err
	}
	defer tx.Rollback()
	var id int64
	if err := tx.QueryRow("SELECT id FROM posts WHERE id = ?", postID).Scan(&id); err != nil {
		return false, err
	}
	result, err := tx.Exec("INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING", actor.userID, postID)
	if err != nil {
		return false, err
	}
	affected, err := result.RowsAffected()
	if err != nil {
		return false, err
	}
	if err := tx.Commit(); err != nil {
		return false, err
	}
	return affected == 0, nil
}
