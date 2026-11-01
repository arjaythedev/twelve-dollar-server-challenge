/-!
# The feed API, as a model

The domain of the challenge (`SPEC.md`, `schema.sql`) written as ordinary Lean data and
functions. This is the source of truth: the invariant, the read plans and the C emitted for
them are all stated against these definitions, and the definitions are executable, so the
generated server is also tested against them (`Feed.Oracle`).

The model mirrors the stored representation: each table is the list of its rows in rowid
order, as SQLite's table B-trees hold them. Timestamps are the stored ISO-8601 strings; their
string order is time order (fixed width, UTC) and agrees with SQLite's BINARY collation
(byte order of UTF-8 is code-point order, which is Lean's `String` order).
-/
namespace Feed

structure User where
  id : Nat
  username : String
  deriving Repr, DecidableEq, Inhabited

structure Post where
  id : Nat
  author : Nat
  body : String
  createdAt : String
  deriving Repr, DecidableEq, Inhabited

structure Like where
  user : Nat
  post : Nat
  deriving Repr, DecidableEq, Inhabited

structure Db where
  users : List User
  posts : List Post
  likes : List Like
  deriving Repr, Inhabited

/-- The post object of the wire contract: `{"id","body","created_at","author","like_count"}`. -/
structure PostView where
  id : Nat
  body : String
  createdAt : String
  author : String
  likeCount : Nat
  deriving Repr, DecidableEq, Inhabited

namespace Db

def user? (db : Db) (uid : Nat) : Option User := db.users.find? (·.id == uid)

def post? (db : Db) (pid : Nat) : Option Post := db.posts.find? (·.id == pid)

/-- `like_count`: the number of rows in `likes` for the post, as of the request. -/
def likeCount (db : Db) (pid : Nat) : Nat := db.likes.countP (·.post == pid)

/-- `posts p JOIN users u ON u.id = p.user_id`, plus the like count: a post whose author row
is missing has no view (the inner join drops it). -/
def view (db : Db) (p : Post) : Option PostView :=
  (db.user? p.author).map fun u => ⟨p.id, p.body, p.createdAt, u.username, db.likeCount p.id⟩

end Db

/-- `ORDER BY created_at DESC, id DESC`: `newerEq a b` when `a` may precede `b`. -/
def newerEq (a b : Post) : Bool :=
  decide (b.createdAt < a.createdAt) || (decide (b.createdAt = a.createdAt) && decide (b.id ≤ a.id))

namespace Db

/-- `GET /feed`: the 20 newest posts (joined), newest first. -/
def feed (db : Db) : List PostView :=
  ((db.posts.mergeSort newerEq).filterMap db.view).take 20

/-- `GET /posts/:id`. -/
def getPost (db : Db) (pid : Nat) : Option PostView :=
  (db.post? pid).bind db.view

/-- SQLite's rowid choice for `INSERT` into a table without AUTOINCREMENT: one more than the
largest rowid in use. -/
def nextPostId (db : Db) : Nat := (db.posts.map (·.id)).foldl max 0 + 1

/-- `POST /posts`: the row the insert adds (its timestamp is the column default, `now`). -/
def newPost (db : Db) (author : Nat) (body now : String) : Post :=
  ⟨db.nextPostId, author, body, now⟩

def addPost (db : Db) (p : Post) : Db := { db with posts := db.posts ++ [p] }

/-- `POST /posts/:id/like`: `none` when the post does not exist; otherwise the new database
and whether the like already existed (one like per user and post). -/
def like (db : Db) (uid pid : Nat) : Option (Db × Bool) :=
  match db.post? pid with
  | none => none
  | some _ =>
    if db.likes.any (fun l => l.user == uid && l.post == pid) then some (db, true)
    else some ({ db with likes := db.likes ++ [⟨uid, pid⟩] }, false)

end Db
end Feed
