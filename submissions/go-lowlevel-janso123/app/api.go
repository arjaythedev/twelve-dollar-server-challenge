// SPDX-License-Identifier: MIT
// Adapted from tbsvttr's Go net/http submission (PR #18) to
// arjaythedev/twelve-dollar-server-challenge, licensed under the repository MIT
// license. Original parsing, HTTP, JSON and SQL design are retained here.
// This experiment calls the same API/SQL directly from a Linux epoll loop.
// Validated writes are handed to a dedicated group-commit writer.
package main

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

type Post struct {
	ID      int64  `json:"id"`
	Body    string `json:"body"`
	Created string `json:"created_at"`
	Author  string `json:"author"`
	Likes   int64  `json:"like_count"`
}
type reply struct {
	status int
	data   []byte
}
type job struct {
	method, path, auth string
	body               []byte
}
type app struct {
	conn       *sql.Conn
	statements map[string]*sql.Stmt
	reader     *cReader
	secret     []byte
	started    time.Time
	submit     func(writeRequest) reply
}

var ctx = context.Background()

func positive(s string) int64 {
	if s == "" {
		return 0
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0
		}
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil || n <= 0 || n > 9007199254740991 {
		return 0
	}
	return n
}
func space(c rune) bool {
	return c >= 9 && c <= 13 || c == 32 || c == 0xa0 || c == 0x1680 ||
		c >= 0x2000 && c <= 0x200a || c == 0x2028 || c == 0x2029 ||
		c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff
}
func (a *app) authenticate(auth string) (int64, string, string) {
	if !strings.HasPrefix(auth, "Bearer ") {
		return 0, "", "missing bearer token"
	}
	invalid := "invalid or expired token"
	token := strings.TrimPrefix(auth, "Bearer ")
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return 0, "", invalid
	}
	decoded := make([][]byte, 3)
	for i, part := range parts {
		for _, c := range part {
			if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-' || c == '_') {
				return 0, "", invalid
			}
		}
		var err error
		decoded[i], err = base64.RawURLEncoding.DecodeString(part)
		if err != nil || len(decoded[i]) == 0 {
			return 0, "", invalid
		}
	}
	mac := hmac.New(sha256.New, a.secret)
	mac.Write([]byte(parts[0] + "." + parts[1]))
	if !hmac.Equal(mac.Sum(nil), decoded[2]) {
		return 0, "", invalid
	}
	var head, claims map[string]any
	if !utf8.Valid(decoded[0]) || !utf8.Valid(decoded[1]) || json.Unmarshal(decoded[0], &head) != nil || json.Unmarshal(decoded[1], &claims) != nil || head["alg"] != "HS256" {
		return 0, "", invalid
	}
	now := float64(time.Now().UnixNano()) / 1e9
	exp, ok := claims["exp"].(float64)
	if !ok || exp <= now {
		return 0, "", invalid
	}
	if value, present := claims["nbf"]; present {
		nbf, ok := value.(float64)
		if !ok || nbf > now {
			return 0, "", invalid
		}
	}
	sub, subOK := claims["sub"].(string)
	name, nameOK := claims["username"].(string)
	id := positive(sub)
	if !subOK || !nameOK || id == 0 {
		return 0, "", "invalid token payload"
	}
	return id, name, ""
}
func (a *app) handle(j job) reply {
	if j.method == "GET" && j.path == "/health" {
		var one int
		if err := a.statements["health"].QueryRow().Scan(&one); err != nil {
			return healthError(err.Error())
		}
		return healthOK(int64(time.Since(a.started).Seconds()))
	}
	create := j.method == "POST" && j.path == "/posts"
	like := j.method == "POST" && strings.HasSuffix(j.path, "/like")
	idText := ""
	if strings.HasPrefix(j.path, "/posts/") && (j.method == "GET" || like) {
		idText = strings.TrimPrefix(j.path, "/posts/")
		if like {
			idText = strings.TrimSuffix(idText, "/like")
		}
		if strings.Contains(idText, "/") {
			idText = ""
		}
	}
	feed := j.method == "GET" && j.path == "/feed"
	if !create && !feed && idText == "" {
		return failure(404, "not found")
	}
	var user int64
	var name, message string
	if j.method == "POST" {
		user, name, message = a.authenticate(j.auth)
		if message != "" {
			return failure(401, message)
		}
	}
	id := positive(idText)
	if !create && !feed && id == 0 {
		return failure(400, "invalid post id")
	}
	if create {
		var value any
		if !utf8.Valid(j.body) || json.Unmarshal(j.body, &value) != nil {
			return failure(400, "malformed JSON body")
		}
		object, _ := value.(map[string]any)
		body, _ := object["body"].(string)
		body = strings.TrimFunc(body, space)
		if body == "" {
			return failure(400, "body is required")
		}
		if utf8.RuneCountInString(body) > 500 {
			return failure(400, "body must be at most 500 characters")
		}
		q := writeRequest{user: user, body: body, author: name, create: true}
		if a.submit != nil {
			return a.submit(q)
		}
		return executeWrite(a.statements, q)
	}
	if like {
		q := writeRequest{user: user, post: id}
		if a.submit != nil {
			return a.submit(q)
		}
		return executeWrite(a.statements, q)
	}
	return a.reader.read(feed, id)
}

func executeWrite(statements map[string]*sql.Stmt, q writeRequest) reply {
	r, _, _ := executeWriteResult(statements, q)
	return r
}

// Return the underlying SQL error as well as the public reply so the writer
// can classify failures without changing any HTTP response bytes.
func executeWriteResult(statements map[string]*sql.Stmt, q writeRequest) (reply, error, string) {
	internal := func() reply { return failure(500, "internal server error") }
	user, body, id := q.user, q.body, q.post
	if q.create {
		p := Post{Body: body, Author: q.author}
		rows, err := statements["create"].Query(user, body)
		if err != nil {
			return internal(), err, "create_query"
		}
		if !rows.Next() {
			err = rows.Err()
			rows.Close()
			if err == nil {
				err = fmt.Errorf("create RETURNING produced no row")
			}
			return internal(), err, "create_next"
		}
		if err := rows.Scan(&p.ID, &p.Created); err != nil {
			rows.Close()
			return internal(), err, "create_scan"
		}
		// Finish RETURNING before COMMIT. Production publishes this provisional
		// reply only after the containing transaction has committed.
		unexpectedRow := rows.Next()
		stepErr, closeErr := rows.Err(), rows.Close()
		if unexpectedRow || stepErr != nil || closeErr != nil {
			if stepErr != nil {
				return internal(), stepErr, "create_next"
			}
			if closeErr != nil {
				return internal(), closeErr, "create_close"
			}
			return internal(), fmt.Errorf("create RETURNING produced extra row"), "create_next"
		}
		return createdPost(p), nil, ""
	}
	{
		result, err := statements["like"].Exec(user, id)
		if err != nil {
			return internal(), err, "like_exec"
		}
		changed, err := result.RowsAffected()
		if err != nil {
			return internal(), err, "like_rows_affected"
		}
		if changed == 0 {
			var one int
			err = statements["exists"].QueryRow(id).Scan(&one)
			if err == sql.ErrNoRows {
				return failure(404, "post not found"), nil, ""
			}
			if err != nil {
				return internal(), err, "like_exists"
			}
		}
		status := 200
		if changed != 0 {
			status = 201
		}
		return likeResult(status, changed == 0, id), nil, ""
	}
}

// Close statements after the actor stops and before closing its connection.
// database/sql statement dependencies can otherwise retain an exclusive lock.
func (a *app) close() error {
	var result error
	if a.reader != nil {
		a.reader.close()
	}
	for _, statement := range a.statements {
		result = errors.Join(result, statement.Close())
	}
	return result
}

func newApp(conn *sql.Conn, secret []byte, path string) (*app, error) {
	a := &app{conn: conn, statements: make(map[string]*sql.Stmt), secret: secret, started: time.Now()}
	const read = "SELECT p.id,p.body,p.created_at,u.username,(SELECT count(*) FROM likes WHERE post_id=p.id) FROM posts p JOIN users u ON u.id=p.user_id"
	for name, query := range map[string]string{
		"create": "INSERT INTO posts(user_id,body) VALUES (?,?) RETURNING id,created_at",
		"like":   "INSERT INTO likes(user_id,post_id) SELECT ?1,?2 WHERE EXISTS(SELECT 1 FROM posts WHERE id=?2) ON CONFLICT(user_id,post_id) DO NOTHING",
		"exists": "SELECT 1 FROM posts WHERE id=?", "health": "SELECT 1",
	} {
		statement, err := conn.PrepareContext(ctx, query)
		if err != nil {
			a.close()
			return nil, err
		}
		a.statements[name] = statement
	}
	reader, err := newCReader(path, read+" ORDER BY p.created_at DESC,p.id DESC LIMIT 20", read+" WHERE p.id=?")
	if err != nil {
		a.close()
		return nil, err
	}
	a.reader = reader
	return a, nil
}
