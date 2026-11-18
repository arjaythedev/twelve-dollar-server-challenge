package main

import "strconv"

// The whole post object is rendered by SQLite, so a request costs one query and the bytes go from
// SQLite's result straight into the response buffer.
const postJSON = `json_object('id',p.id,'body',p.body,'created_at',p.created_at,'author',u.username,` +
	`'like_count',(SELECT count(*) FROM likes l WHERE l.post_id = p.id))`

const (
	// The subquery yields plain columns and the object is built inside the aggregate: building it in
	// the subquery would lose the JSON subtype and need a json() re-parse (+22% CPU, measured).
	// ORDER BY inside the aggregate pins the array order whatever plan SQLite picks.
	sqlFeed = `SELECT '{"posts":' || json_group_array(` +
		`json_object('id',id,'body',body,'created_at',ca,'author',username,'like_count',lc) ORDER BY ca DESC, id DESC) || '}' FROM (` +
		`SELECT p.id AS id, p.body AS body, p.created_at AS ca, u.username AS username, ` +
		`(SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS lc ` +
		`FROM posts p JOIN users u ON u.id = p.user_id ` +
		`ORDER BY p.created_at DESC, p.id DESC LIMIT 20)`
	sqlPost = `SELECT json_object('post', ` + postJSON + `) FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1`
	// ?2 is the token username: author is taken from the token, not looked up (SPEC).
	sqlCreate = `INSERT INTO posts (user_id, body) VALUES (?1, ?3) RETURNING ` +
		`json_object('post', json_object('id',id,'body',body,'created_at',created_at,'author',?2,'like_count',0))`
	// Inserts only if the post exists: 0 changes means "already liked" or "no such post".
	sqlLike = `INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ` +
		`ON CONFLICT (user_id, post_id) DO NOTHING`
	sqlExists = `SELECT 1 FROM posts WHERE id = ?1`
	sqlPing   = `SELECT 1`
)

var pragmas = []string{
	"PRAGMA journal_mode=WAL",
	"PRAGMA synchronous=NORMAL",
	"PRAGMA mmap_size=1073741824",
	"PRAGMA cache_size=-2048",
	"PRAGMA wal_autocheckpoint=0",
}

const (
	maxBatch   = 256  // requests drained per loop pass; writes among them share one transaction
	ckptFrames = 1000 // WAL frames (4 KiB pages) before an inline TRUNCATE checkpoint
	outCap     = 16 << 10
)

type reqKind uint8

const (
	reqFeed reqKind = iota
	reqPost
	reqCreate
	reqLike
	reqPing
)

// dbReq is one unit of work for the DB goroutine. Handlers fill it, send it, wait on done, then
// read status/out. Buffers are owned by the handler and reused through a pool.
type dbReq struct {
	kind   reqKind
	id     int64  // post id
	user   int64  // user id from the token
	body   []byte // create: trimmed post body
	name   []byte // create: token username
	out    []byte // response body is appended here
	status int    // 200/201/404/500; 0 means "not handled"
	done   chan struct{}
}

type db struct {
	c      *conn
	ch     chan *dbReq
	writes []*dbReq
}

// openDB opens the database and starts the DB goroutine.
func openDB(path string) (*db, error) {
	c, err := openConn(path)
	if err != nil {
		return nil, err
	}
	d := &db{c: c, ch: make(chan *dbReq, 4096), writes: make([]*dbReq, 0, maxBatch)}
	go d.loop()
	return d, nil
}

// loop is the only goroutine that touches SQLite. Each pass drains the queue, answers reads at once
// and commits every queued write in one transaction (group commit) before releasing those requests,
// so a 201 always means the row is on disk per synchronous=NORMAL.
func (d *db) loop() {
	for {
		r := <-d.ch
		d.writes = d.writes[:0]
		for n := 0; ; {
			if r.kind == reqCreate || r.kind == reqLike {
				d.writes = append(d.writes, r)
			} else {
				d.read(r)
				r.done <- struct{}{}
			}
			n++
			if n >= maxBatch {
				break
			}
			select {
			case r = <-d.ch:
			default:
				n = maxBatch
			}
			if n >= maxBatch {
				break
			}
		}
		if len(d.writes) > 0 {
			d.commitBatch()
		}
		if len(d.ch) == 0 && d.c.walFrames() > ckptFrames {
			d.c.checkpoint()
		}
	}
}

func (d *db) read(r *dbReq) {
	var (
		rows int
		err  error
	)
	switch r.kind {
	case reqFeed:
		r.out, rows, err = d.c.run(qFeed, 0, -1, nil, nil, r.out)
	case reqPost:
		r.out, rows, err = d.c.run(qPost, r.id, -1, nil, nil, r.out)
	case reqPing:
		r.out, rows, err = d.c.run(qPing, 0, -1, nil, nil, r.out[:0])
		r.out = r.out[:0]
	}
	switch {
	case err != nil:
		r.status = 500
	case rows == 0:
		r.status = 404
	default:
		r.status = 200
	}
}

func (d *db) step(q query) error {
	_, err := d.c.exec(q, 0, 0)
	return err
}

func (d *db) commitBatch() {
	if err := d.step(qBegin); err != nil {
		d.failWrites()
		return
	}
	for _, r := range d.writes {
		d.write(r)
	}
	if err := d.step(qCommit); err != nil {
		_ = d.step(qRollback)
		d.failWrites()
		return
	}
	for _, r := range d.writes {
		r.done <- struct{}{}
	}
}

func (d *db) failWrites() {
	for _, r := range d.writes {
		r.status, r.out = 500, r.out[:0]
		r.done <- struct{}{}
	}
}

func (d *db) write(r *dbReq) {
	switch r.kind {
	case reqCreate:
		out, rows, err := d.c.run(qCreate, r.user, -1, r.name, r.body, r.out)
		r.out = out
		switch {
		case err != nil || rows == 0:
			r.status = 500
		default:
			r.status = 201
		}
	case reqLike:
		n, err := d.c.exec(qLike, r.user, r.id)
		switch {
		case err != nil:
			r.status = 500
			return
		case n == 1:
			r.status = 201
		default:
			_, rows, err := d.c.run(qExists, r.id, -1, nil, nil, r.out[:0])
			switch {
			case err != nil:
				r.status = 500
				return
			case rows == 0:
				r.status = 404
				return
			}
			r.status = 200
		}
		r.out = append(r.out, `{"liked":true,"already_liked":`...)
		r.out = strconv.AppendBool(r.out, r.status == 200)
		r.out = append(r.out, `,"post_id":`...)
		r.out = strconv.AppendInt(r.out, r.id, 10)
		r.out = append(r.out, '}')
	}
}
