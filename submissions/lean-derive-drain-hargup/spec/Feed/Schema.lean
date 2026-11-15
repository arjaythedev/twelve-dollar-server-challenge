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

/-- Position of a column in the stored record. -/
def Table.ordinal (t : Table) (c : String) : Nat := (t.cols.findIdx? (·.name == c)).getD 0

def Table.column (t : Table) (c : String) : Column := (t.cols.find? (·.name == c)).getD ⟨c, .text, false⟩

end Feed.Schema
