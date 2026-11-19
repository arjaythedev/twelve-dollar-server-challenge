package main

import "core:fmt"
import "core:os"
import "core:strings"
import "core:time"

// SQLite is built with profile-guided optimisation (see build.sh). The training run is this
// program built with -define:PGO_TRAIN=true: it fills a synthetic database (never the seed) and
// runs the server's own statements in roughly the load test's mix.
PGO_TRAIN :: #config(PGO_TRAIN, false)

when PGO_TRAIN {
	@(private = "file")
	PGO_USERS :: 5_000

	@(private = "file")
	PGO_POSTS :: 50_000

	// The challenge's fixed schema.sql, copied so the build needs nothing outside this folder.
	@(private = "file")
	PGO_DATA :: `
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
BEGIN;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 5000)
INSERT INTO users (id, username) SELECT i, 'user' || i FROM n;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 50000)
INSERT INTO posts (id, user_id, body, created_at)
SELECT i, 1 + abs(random()) % 5000, 'post ' || i || ' ' || hex(randomblob(abs(random()) % 100)),
       strftime('%Y-%m-%dT%H:%M:%fZ', '2025-01-01', '+' || i || ' seconds') FROM n;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000)
INSERT OR IGNORE INTO likes (user_id, post_id) SELECT 1 + abs(random()) % 5000, 1 + abs(random()) % 50000 FROM n;
COMMIT;`

	pgo_train :: proc(path: string) {
		start := time.tick_now()
		tmp: Sqlite3
		if open_v2(strings.clone_to_cstring(path), &tmp, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nil) != SQLITE_OK ||
		   exec(tmp, PGO_DATA, nil, nil, nil) != SQLITE_OK {
			fmt.eprintfln("pgo: %s", errmsg(tmp))
			os.exit(1)
		}
		close(tmp)
		shutdown() // so db_open configures SQLite's page cache exactly as the server does
		db_open(path)

		posts := i64(PGO_POSTS)
		for i in 0 ..< 50_000 {
			out_reset()
			write_feed()
			out_reset()
			write_post_by_id(1 + i64(i) * 7919 % posts)
			if i % 7 == 0 {
				post := posts - i64(i % 20)
				post_exists(post)
				insert_like(1 + i64(i) % PGO_USERS, post)
			}
			if i % 50 == 0 {
				out_reset()
				if create_post(1 + i64(i) % PGO_USERS, "a post written while training the profile", "trainer") { posts += 1 }
			}
		}
		fmt.printfln("pgo: trained in %v", time.tick_since(start))
	}
}
