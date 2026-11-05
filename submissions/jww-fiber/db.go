package main

import (
	"database/sql/driver"
	"fmt"
	"io"

	"github.com/mattn/go-sqlite3"
)

const dsnParams = "?_journal_mode=WAL" +
	"&_synchronous=NORMAL" +
	"&_busy_timeout=5000" +
	"&_cache_size=-65536" +
	"&_foreign_keys=on"

// feedSQL builds the whole {"posts":[...]} envelope inside SQLite. That turns
// 20 rows x 5 columns of cgo reads into a single text column, and SQLite's JSON
// escaping matches the golden responses byte for byte. The LIMIT subquery is
// materialised in index order, so json_group_array sees rows newest first.
const feedSQL = `SELECT '{"posts":' || json_group_array(json_object(
	'id', id, 'body', body, 'created_at', created_at,
	'author', username, 'like_count', like_count)) || '}'
	FROM (
		SELECT p.id AS id, p.body AS body, p.created_at AS created_at,
			u.username AS username,
			(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
		FROM posts p INDEXED BY posts_created_at_id_idx
		JOIN users u ON u.id = p.user_id
		ORDER BY p.created_at DESC, p.id DESC LIMIT 20
	)`

const postSQL = `SELECT '{"post":' || json_object(
	'id', p.id, 'body', p.body, 'created_at', p.created_at,
	'author', u.username, 'like_count',
	(SELECT count(*) FROM likes l WHERE l.post_id = p.id)) || '}'
	FROM posts p JOIN users u ON u.id = p.user_id
	WHERE p.id = ?`

const insertPostSQL = `INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at`

const insertLikeSQL = `INSERT INTO likes (user_id, post_id)
	SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?)
	ON CONFLICT (user_id, post_id) DO NOTHING`

const postExistsSQL = `SELECT 1 FROM posts WHERE id = ?`

// dbConn owns one SQLite connection and the prepared statements that run on it.
// A dbConn must only be used by one goroutine at a time; dbPool enforces that.
type dbConn struct {
	conn *sqlite3.SQLiteConn

	feed       *sqlite3.SQLiteStmt
	post       *sqlite3.SQLiteStmt
	health     *sqlite3.SQLiteStmt
	insertPost *sqlite3.SQLiteStmt
	insertLike *sqlite3.SQLiteStmt
	postExists *sqlite3.SQLiteStmt
}

func openDBConn(path string) (*dbConn, error) {
	drv := &sqlite3.SQLiteDriver{}
	raw, err := drv.Open("file:" + path + dsnParams)
	if err != nil {
		return nil, err
	}
	conn, ok := raw.(*sqlite3.SQLiteConn)
	if !ok {
		_ = raw.Close()
		return nil, fmt.Errorf("unexpected sqlite driver connection type %T", raw)
	}

	if _, err := conn.Exec("PRAGMA temp_store = MEMORY", nil); err != nil {
		_ = conn.Close()
		return nil, err
	}
	if _, err := conn.Exec("PRAGMA mmap_size = 268435456", nil); err != nil {
		_ = conn.Close()
		return nil, err
	}

	c := &dbConn{conn: conn}
	prep := func(query string) (*sqlite3.SQLiteStmt, error) {
		s, err := conn.Prepare(query)
		if err != nil {
			return nil, err
		}
		return s.(*sqlite3.SQLiteStmt), nil
	}
	if c.feed, err = prep(feedSQL); err != nil {
		c.close()
		return nil, err
	}
	if c.post, err = prep(postSQL); err != nil {
		c.close()
		return nil, err
	}
	if c.health, err = prep(`SELECT 1`); err != nil {
		c.close()
		return nil, err
	}
	if c.insertPost, err = prep(insertPostSQL); err != nil {
		c.close()
		return nil, err
	}
	if c.insertLike, err = prep(insertLikeSQL); err != nil {
		c.close()
		return nil, err
	}
	if c.postExists, err = prep(postExistsSQL); err != nil {
		c.close()
		return nil, err
	}
	return c, nil
}

func (c *dbConn) close() {
	for _, s := range []*sqlite3.SQLiteStmt{
		c.feed, c.post, c.health, c.insertPost, c.insertLike, c.postExists,
	} {
		if s != nil {
			_ = s.Close()
		}
	}
	if c.conn != nil {
		_ = c.conn.Close()
	}
}

func valueInt64(v driver.Value) int64 {
	if n, ok := v.(int64); ok {
		return n
	}
	return 0
}

func valueString(v driver.Value) string {
	if s, ok := v.(string); ok {
		return s
	}
	return ""
}

// queryFeed returns the complete {"posts":[...]} response body. json_group_array
// always yields one row, even when the feed is empty (it yields "[]").
func (c *dbConn) queryFeed() (string, error) {
	rows, err := c.feed.Query(nil)
	if err != nil {
		return "", err
	}
	defer rows.Close()

	var dest [1]driver.Value
	if err := rows.Next(dest[:]); err != nil {
		if err == io.EOF {
			return `{"posts":[]}`, nil
		}
		return "", err
	}
	body := valueString(dest[0])
	if err := rows.Close(); err != nil {
		return "", err
	}
	return body, nil
}

// queryPost returns the complete {"post":{...}} response body.
func (c *dbConn) queryPost(id int64) (string, bool, error) {
	rows, err := c.post.Query([]driver.Value{id})
	if err != nil {
		return "", false, err
	}
	defer rows.Close()

	var dest [1]driver.Value
	err = rows.Next(dest[:])
	if err == io.EOF {
		return "", false, nil
	}
	if err != nil {
		return "", false, err
	}
	body := valueString(dest[0])
	if err := rows.Close(); err != nil {
		return "", false, err
	}
	return body, true, nil
}

func (c *dbConn) ping() error {
	rows, err := c.health.Query(nil)
	if err != nil {
		return err
	}
	defer rows.Close()

	var dest [1]driver.Value
	err = rows.Next(dest[:])
	if err == io.EOF {
		return nil
	}
	return err
}

// createPost inserts one row and does not return until the statement has been
// fully stepped and reset, so the row is committed before the 201 is sent.
func (c *dbConn) createPost(userID int64, body string) (int64, string, error) {
	rows, err := c.insertPost.Query([]driver.Value{userID, body})
	if err != nil {
		return 0, "", err
	}

	var dest [2]driver.Value
	if err := rows.Next(dest[:]); err != nil {
		_ = rows.Close()
		return 0, "", err
	}
	id := valueInt64(dest[0])
	createdAt := valueString(dest[1])

	// A single-row INSERT ... RETURNING has one row. Step to DONE and reset so
	// the write is committed before we respond.
	var extra [2]driver.Value
	for {
		e := rows.Next(extra[:])
		if e == io.EOF {
			break
		}
		if e != nil {
			_ = rows.Close()
			return 0, "", e
		}
	}
	if err := rows.Close(); err != nil {
		return 0, "", err
	}
	return id, createdAt, nil
}

func (c *dbConn) createLike(userID, postID int64) (int64, error) {
	res, err := c.insertLike.Exec([]driver.Value{userID, postID, postID})
	if err != nil {
		return 0, err
	}
	return res.RowsAffected()
}

func (c *dbConn) existsPost(id int64) (bool, error) {
	rows, err := c.postExists.Query([]driver.Value{id})
	if err != nil {
		return false, err
	}
	defer rows.Close()

	var dest [1]driver.Value
	err = rows.Next(dest[:])
	if err == io.EOF {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	return true, nil
}

// dbPool is a fixed set of connections. Each request borrows one connection,
// runs its statements, and returns it. SQLite connections are not safe for
// concurrent use, so the channel is the ownership hand-off.
type dbPool struct {
	path  string
	conns chan *dbConn
}

func newDBPool(path string, size int) (*dbPool, error) {
	p := &dbPool{path: path, conns: make(chan *dbConn, size)}
	for i := 0; i < size; i++ {
		c, err := openDBConn(path)
		if err != nil {
			p.close()
			return nil, err
		}
		p.conns <- c
	}
	return p, nil
}

func (p *dbPool) get() *dbConn {
	return <-p.conns
}

func (p *dbPool) put(c *dbConn) {
	p.conns <- c
}

func (p *dbPool) close() {
	for i := 0; i < cap(p.conns); i++ {
		select {
		case c := <-p.conns:
			c.close()
		default:
			return
		}
	}
}
