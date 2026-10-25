package main

/*
#include <stdlib.h>
*/
import "C"

// The training run for the profile-guided build.
//
// Two profiles come out of one pass. GCC writes .gcda counters for the SQLite amalgamation,
// which is the half of the CPU that is not Go, and the Go runtime writes default.pgo for the
// handlers, the JSON writer, the parser and the JWT path. build.sh then compiles both again
// against what this measured.
//
// The workload is the one in bench/load.js, driven straight through parseRequest, handle and
// frameResponse rather than over a socket. Nothing here needs a network, a seed database or a
// load generator, so the whole thing runs inside build.sh in a few seconds on the box being
// built on - which is the point, because a profile from a different machine would be training
// for the wrong branch predictor and the wrong cache.

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"os"
	"path/filepath"
	"runtime/pprof"
	"strconv"
	"time"
)

// trainSchema is a copy of the repository's schema.sql, which the rules fix: submissions may not
// add or change tables, columns, indexes or triggers. Copied rather than read from ../../ so a
// build does not depend on where the submission sits.
const trainSchema = `
CREATE TABLE users (
  id          INTEGER PRIMARY KEY,
  username    TEXT NOT NULL UNIQUE,
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE TABLE posts (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id),
  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE TABLE likes (
  user_id     INTEGER NOT NULL REFERENCES users(id),
  post_id     INTEGER NOT NULL REFERENCES posts(id),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  PRIMARY KEY (user_id, post_id)
);
CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX posts_user_id_idx       ON posts (user_id);
CREATE INDEX likes_post_id_idx       ON likes (post_id);
`

// A fifth of the seed database's shape, which is enough to give the B-trees their real depth
// without making the seed the slow part of the build.
const (
	trainUsers = 5000
	trainPosts = 100000
	trainLikes = 400000

	// The workload runs for a fixed time rather than a fixed count, because the Go profiler
	// samples at 100 Hz: a count tuned on one machine would give a thin profile on a slower
	// one. trainMaxIterations only stops a pathologically fast machine spinning.
	trainDuration      = 12 * time.Second
	trainMaxIterations = 2000000
)

// runTraining builds a scratch database, drives the request path over it, and writes the Go
// profile. It never returns: the process has to leave through C so GCC's atexit handler gets to
// write the counters, which Go's own exit path would skip.
func runTraining(cfg config) error {
	dir, err := os.MkdirTemp("", "train")
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)

	d, err := openDBCreate(filepath.Join(dir, "train.db"), trainSchema)
	if err != nil {
		return err
	}
	if err := seedTraining(d); err != nil {
		return err
	}

	s := &server{
		db:         d,
		ver:        newVerifier([]byte(cfg.secret)),
		started:    time.Now(),
		trim:       make([]byte, 0, 4096),
		likeCounts: make([]int32, maxFeedSpan),
	}

	profile, err := os.Create("default.pgo")
	if err != nil {
		return err
	}
	if err := pprof.StartCPUProfile(profile); err != nil {
		return err
	}
	n := driveWorkload(s, cfg.secret)
	pprof.StopCPUProfile()
	if err := profile.Close(); err != nil {
		return err
	}
	fmt.Fprintf(os.Stderr, "train: %d requests, profile in default.pgo\n", n)

	// os.Exit would skip the C atexit handler that writes the .gcda counters.
	C.exit(0)
	return nil
}

func seedTraining(d *db) error {
	// Three statements, not three hundred thousand: a recursive CTE generates the rows, so the
	// seed is not the slowest part of the build. generate_series is not in the amalgamation.
	const seq = `WITH RECURSIVE seq(value) AS (
	    SELECT 1 UNION ALL SELECT value + 1 FROM seq WHERE value < %d)`
	stmts := []string{
		"BEGIN",
		fmt.Sprintf("INSERT INTO users (id, username) "+seq+
			" SELECT value, 'user_' || value FROM seq", trainUsers),
		fmt.Sprintf("INSERT INTO posts (id, user_id, body, created_at) "+seq+`
		    SELECT value, 1 + (value * 7) %% %d,
		           'training post ' || value || ' with enough text to look like a real body',
		           strftime('%%Y-%%m-%%dT%%H:%%M:%%fZ', '2025-01-01', '+' || value || ' minutes')
		      FROM seq`, trainPosts, trainUsers),
		fmt.Sprintf("INSERT OR IGNORE INTO likes (user_id, post_id) "+seq+`
		    SELECT 1 + (value * 13) %% %d, 1 + (value * 3) %% %d FROM seq`,
			trainLikes, trainUsers, trainPosts),
		"COMMIT",
	}
	for _, sql := range stmts {
		if err := d.exec(sql); err != nil {
			return fmt.Errorf("seed: %w", err)
		}
	}
	return nil
}

// driveWorkload runs the request mix from bench/load.js: a feed read, a post read, a like on
// about one in seven, and a new post on about one in fifty.
func driveWorkload(s *server, secret string) int {
	token := trainToken(secret, 1, "user_1")
	// maxRespBytes, not maxBodyBytes: a legal /feed overflows the request bound (see api.go), so
	// the profile would cover the overflow path rather than the one that runs.
	out := make([]byte, hdrReserve+maxRespBytes)
	raw := make([]byte, 0, 1024)
	done := 0
	deadline := time.Now().Add(trainDuration)

	// The same shape the loop uses: the body is written after the header reserve and the header
	// is framed backwards into it, so the profile covers the real buffer arithmetic.
	var p parsed
	emit := func(req []byte) {
		if parseRequest(req, &p) != parseDone {
			return
		}
		status, body := s.handle(&p.req, out[hdrReserve:hdrReserve:len(out)])
		frameResponse(out, len(body), status, p.keepAlive)
		done++
	}

	for i := 0; i < trainMaxIterations; i++ {
		if i%256 == 0 && time.Now().After(deadline) {
			break
		}
		emit(trainRequest(raw[:0], "GET", "/feed", "", ""))
		id := strconv.Itoa(1 + (i*37)%trainPosts)
		emit(trainRequest(raw[:0], "GET", "/posts/"+id, "", ""))
		if i%7 == 0 {
			emit(trainRequest(raw[:0], "POST", "/posts/"+id+"/like", token, ""))
		}
		if i%50 == 0 {
			emit(trainRequest(raw[:0], "POST", "/posts", token,
				`{"body":"a training post body with \"quotes\" and some length to it"}`))
		}
	}
	return done
}

func trainRequest(buf []byte, method, path, token, body string) []byte {
	buf = append(buf, method...)
	buf = append(buf, ' ')
	buf = append(buf, path...)
	buf = append(buf, " HTTP/1.1\r\nHost: localhost\r\nUser-Agent: k6/2.3.0\r\nAccept: */*\r\n"...)
	if token != "" {
		buf = append(buf, "Authorization: Bearer "...)
		buf = append(buf, token...)
		buf = append(buf, '\r', '\n')
	}
	if body != "" {
		buf = append(buf, "Content-Type: application/json\r\nContent-Length: "...)
		buf = appendInt(buf, int64(len(body)))
		buf = append(buf, '\r', '\n')
	}
	buf = append(buf, '\r', '\n')
	return append(buf, body...)
}

func trainToken(secret string, userID int, username string) string {
	enc := base64.RawURLEncoding
	header := enc.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))
	payload := enc.EncodeToString([]byte(fmt.Sprintf(
		`{"sub":"%d","username":"%s","iat":%d,"exp":%d}`,
		userID, username, time.Now().Unix(), time.Now().Add(time.Hour).Unix())))
	mac := hmac.New(sha256.New, []byte(secret))
	mac.Write([]byte(header + "." + payload))
	return header + "." + payload + "." + enc.EncodeToString(mac.Sum(nil))
}
