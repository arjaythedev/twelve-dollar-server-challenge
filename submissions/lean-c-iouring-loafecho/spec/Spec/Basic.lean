/-!
# Spec.Basic — data model, reference queries, B-tree plans, invariant

Models the SQLite schema served by the C server:
  users(id, username), posts(id, user_id, body, created_at), likes(user_id, post_id).
`posts` is a rowid table: `id` is the rowid and rows are stored in ascending id order.
-/

structure Post where
  id : Nat
  userId : Nat
  body : String
  createdAt : String
deriving Repr, DecidableEq

structure User where
  id : Nat
  username : String
deriving Repr, DecidableEq

structure Like where
  userId : Nat
  postId : Nat
deriving Repr, DecidableEq

structure Db where
  /-- Posts in rowid (ascending id) order. -/
  posts : List Post
  users : List User
  likes : List Like
deriving Repr

structure PostOut where
  id : Nat
  body : String
  createdAt : String
  author : String
  likeCount : Nat
deriving Repr, DecidableEq

/-- `newer a b` : `a` sorts before `b` under `ORDER BY created_at DESC, id DESC`. -/
def newer (a b : Post) : Bool :=
  decide (b.createdAt < a.createdAt ∨ (b.createdAt = a.createdAt ∧ b.id ≤ a.id))

namespace Db

/-- Username of the first user with id `uid` (`""` if none). -/
def author (db : Db) (uid : Nat) : String :=
  match db.users.find? (·.id = uid) with
  | some u => u.username
  | none => ""

/-- Number of likes rows with `post_id = pid`. -/
def likeCount (db : Db) (pid : Nat) : Nat :=
  db.likes.countP (·.postId = pid)

def render (db : Db) (p : Post) : PostOut :=
  { id := p.id, body := p.body, createdAt := p.createdAt,
    author := db.author p.userId, likeCount := db.likeCount p.id }

/-- REFERENCE: `SELECT … ORDER BY created_at DESC, id DESC LIMIT 20`, rendered. -/
def feed (db : Db) : List PostOut :=
  ((db.posts.mergeSort newer).take 20).map db.render

/-- REFERENCE: `SELECT … WHERE id = ?`, rendered. -/
def getPost (db : Db) (id : Nat) : Option PostOut :=
  (db.posts.find? (·.id = id)).map db.render

/-- B-tree cursor walking backwards: `cur` is the cursor position (rows `0..cur-1` remain
before it), `fuel` is how many more rows we may still collect. -/
def walkBack (posts : List Post) : (fuel : Nat) → (cur : Nat) → List Post
  | 0, _ => []
  | _ + 1, 0 => []
  | k + 1, i + 1 =>
    match posts[i]? with
    | some p => p :: walkBack posts k i
    | none => []

/-- PLAN: start the cursor at the last row of the posts B-tree, step back collecting 20. -/
def feedPlan (db : Db) : List PostOut :=
  (walkBack db.posts 20 db.posts.length).map db.render

/-- PLAN: rowid seek, modelled positionally. -/
def postPlan (db : Db) (id : Nat) : Option PostOut :=
  if id ≥ 1 then (db.posts[id - 1]?).map db.render else none

/-- Invariant under which the plans agree with the references. -/
def Inv (db : Db) : Prop :=
  -- dense: rowids are exactly 1, 2, …, n in storage order
  (∀ i (h : i < db.posts.length), db.posts[i].id = i + 1) ∧
  -- mono: created_at is non-decreasing in rowid order
  db.posts.Pairwise (fun a b => a.createdAt ≤ b.createdAt) ∧
  -- authors: every post has an author row
  (∀ p ∈ db.posts, ∃ u ∈ db.users, u.id = p.userId)

/-- Single pass: ids are `n, n+1, …`. -/
def denseFrom : Nat → List Post → Bool
  | _, [] => true
  | n, p :: ps => p.id == n && denseFrom (n + 1) ps

/-- Single pass: adjacent created_at values are non-decreasing. -/
def monoB : List Post → Bool
  | [] => true
  | [_] => true
  | a :: b :: rest => decide (a.createdAt ≤ b.createdAt) && monoB (b :: rest)

def checkInv (db : Db) : Bool :=
  denseFrom 1 db.posts && monoB db.posts &&
    db.posts.all (fun p => db.users.any (·.id == p.userId))

/-- What the C code checks for a freshly inserted post. -/
def insertOk (db : Db) (p : Post) : Bool :=
  p.id == db.posts.length + 1 &&
  (match db.posts.getLast? with
   | none => true
   | some q => decide (q.createdAt ≤ p.createdAt)) &&
  db.users.any (·.id == p.userId)

def addPost (db : Db) (p : Post) : Db := { db with posts := db.posts ++ [p] }

def addLike (db : Db) (l : Like) : Db := { db with likes := db.likes ++ [l] }

end Db
