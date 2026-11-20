/-!
# The stored schema

`schema.sql` as data. The emitter derives record layouts (which field of a B-tree record holds
which column; a rowid alias is stored as NULL), the SQL text of the write statements, and a
startup check that the database really has this layout (`PRAGMA table_info`), so the
generated B-tree plans never read a table they were not derived for.
-/
namespace Feed.Schema

inductive Ty where | integer | text
  deriving Repr, DecidableEq

def Ty.sql : Ty → String
  | .integer => "INTEGER"
  | .text => "TEXT"

structure Column where
  name : String
  ty : Ty
  /-- `INTEGER PRIMARY KEY`: the column is the rowid; the record stores NULL in its place. -/
  rowid : Bool := false
  deriving Repr

structure Table where
  name : String
  cols : List Column
  deriving Repr

structure Index where
  name : String
  table : String
  keys : List String
  deriving Repr

def users : Table := ⟨"users", [⟨"id", .integer, true⟩, ⟨"username", .text, false⟩, ⟨"created_at", .text, false⟩]⟩
def posts : Table := ⟨"posts", [⟨"id", .integer, true⟩, ⟨"user_id", .integer, false⟩, ⟨"body", .text, false⟩, ⟨"created_at", .text, false⟩]⟩
def likes : Table := ⟨"likes", [⟨"user_id", .integer, false⟩, ⟨"post_id", .integer, false⟩, ⟨"created_at", .text, false⟩]⟩

def tables : List Table := [users, posts, likes]

/-- `CREATE INDEX likes_post_id_idx ON likes (post_id)`: entries are (post_id, rowid). -/
def likesByPost : Index := ⟨"likes_post_id_idx", "likes", ["post_id"]⟩
/-- `PRIMARY KEY (user_id, post_id)` on likes. -/
def likesKey : List String := ["user_id", "post_id"]

/-- A field belongs to this table and has the requested declared type. Stored serial
values remain untrusted and are checked by the generated runtime decoder. -/
structure Field (t : Table) (ty : Ty) where
  index : Fin t.cols.length
  typed : (t.cols[index]).ty = ty

def Field.name (f : Field t ty) : String := t.cols[f.index].name

def postsUser : Field posts .integer := ⟨⟨1, by decide⟩, rfl⟩
def postsBody : Field posts .text := ⟨⟨2, by decide⟩, rfl⟩
def postsCreated : Field posts .text := ⟨⟨3, by decide⟩, rfl⟩
def usersName : Field users .text := ⟨⟨1, by decide⟩, rfl⟩

/-- This prefix locates the field; trailing columns are not demanded. -/
def Field.prefix (f : Field t ty) : Nat := f.index.val + 1

theorem Field.prefix_le (f : Field t ty) : f.prefix ≤ t.cols.length := by
  have := f.index.isLt
  unfold Field.prefix
  omega

end Feed.Schema
