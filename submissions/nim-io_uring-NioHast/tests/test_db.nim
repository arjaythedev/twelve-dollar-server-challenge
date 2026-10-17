## Unit test for src/db.nim against a copy of the challenge seed database.
##
## The seed DB is never touched: we copy it to a temp file and open the copy.
## Each case prints "PASS <name>"; the first failure prints "FAIL <name>" and
## exits non-zero.

import std/[os, strutils]
import ../src/db
import ../src/model

var failures = 0

proc pass(name: string) =
  echo "PASS ", name

proc fail(name, detail: string) =
  inc failures
  echo "FAIL ", name, ": ", detail
  quit(1)

proc expect(name: string, cond: bool, detail: string) =
  if cond: pass(name) else: fail(name, detail)

proc runTests() =
  # locate the seed DB relative to this source file, copy it to a temp file.
  let testDir = currentSourcePath().parentDir               # .../tests
  let submissionDir = testDir.parentDir                     # .../nim-io_uring-NioHast
  let defaultSeed = submissionDir.parentDir.parentDir / "seed" / "feed.db"
  let seedDb = getEnv("SEED_DB", defaultSeed)

  if not fileExists(seedDb):
    quit("seed db not found: " & seedDb, 1)

  let tmpDb = getTempDir() / ("feed_test_" & $getCurrentProcessId() & ".db")
  copyFile(seedDb, tmpDb)
  echo "# seed   = ", seedDb
  echo "# copy   = ", tmpDb

  defer:
    for suffix in ["", "-wal", "-shm"]:
      let f = tmpDb & suffix
      if fileExists(f):
        try: removeFile(f) except OSError: discard

  let db = openDb(tmpDb)

  # health ------------------------------------------------------------------
  expect("health", db.health(), "health() returned false")

  # feed: newest 20 ---------------------------------------------------------
  var posts: seq[Post]
  db.feed(posts)
  echo "# feed[0].id = ", posts[0].id, " (len=", posts.len, ")"
  expect("feed_len_20", posts.len == 20, "len=" & $posts.len)
  expect("feed_first_id_500000", posts[0].id == 500000, "id=" & $posts[0].id)

  # getPost(1) --------------------------------------------------------------
  var p1: Post
  let found1 = db.getPost(1, p1)
  echo "# post1 author=", p1.author, " likes=", p1.likeCount,
       " created=", p1.createdAt
  expect("getPost_1_found", found1, "getPost(1) returned false")
  expect("getPost_1_author", p1.author == "happy_nebula_10241",
         "author=" & p1.author)
  expect("getPost_1_likeCount", p1.likeCount == 11, "likeCount=" & $p1.likeCount)
  expect("getPost_1_createdAt",
         p1.createdAt.startsWith("2025-01-01T00:00:34"),
         "createdAt=" & p1.createdAt)

  # getPost(missing) --------------------------------------------------------
  var missing: Post
  let foundMissing = db.getPost(999999999'i64, missing)
  expect("getPost_missing_false", not foundMissing,
         "getPost(999999999) returned true")

  # createPost --------------------------------------------------------------
  var created: Post
  let createdOk = db.createPost(1, "golden_ember_1", "hello", created)
  echo "# created id=", created.id, " createdAt=", created.createdAt
  expect("createPost_true", createdOk, "createPost returned false")

  var reread: Post
  let rereadOk = db.getPost(created.id, reread)
  expect("createPost_reread_found", rereadOk,
         "getPost(new id) returned false")
  expect("createPost_body", reread.body == "hello", "body=" & reread.body)
  expect("createPost_likeCount", reread.likeCount == 0,
         "likeCount=" & $reread.likeCount)
  expect("createPost_author", reread.author == "golden_ember_1",
         "author=" & reread.author)

  # like --------------------------------------------------------------------
  let like1 = db.like(1, 1)
  echo "# like(1,1) first = ", like1
  expect("like_first_liked", like1 == loLiked, "got " & $like1)

  let like2 = db.like(1, 1)
  echo "# like(1,1) second = ", like2
  expect("like_second_already", like2 == loAlready, "got " & $like2)

  let like3 = db.like(1, 999999999'i64)
  echo "# like(1,missing) = ", like3
  expect("like_missing_nopost", like3 == loNoPost, "got " & $like3)

runTests()

if failures == 0:
  echo "ALL TESTS PASSED"
else:
  echo "FAILURES: ", failures
  quit(1)
