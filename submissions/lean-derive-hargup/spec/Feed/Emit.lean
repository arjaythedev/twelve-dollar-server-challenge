import Feed.Contract
import Feed.Schema
import Feed.Plan

/-!
# Emitting the application layer

`app_gen.c` is produced from the contract (`Feed.Contract`), the schema (`Feed.Schema`) and the
read plans (`Feed.Plan`). Fixed algorithmic skeletons are templates; everything they are
specialized by is computed here:

* serializers from JSON shapes: all constant bytes merged, buffer room computed;
* every constant response, byte for byte, with its `Content-Length`;
* route matching and the order of checks per route;
* record field positions and type checks from the schema, and a startup schema check;
* SQL text for writes and for the fallback feed;
* the invariant check (`Db.checkFrom`) and the post-insert check (`Db.insertOk`);
* the feed window (`Db.window`) and page size.

The runtime (`runtime/*.c`) is application independent: io_uring transport, HTTP framing,
JSON input scanning, HS256, output buffers and SQLite B-tree cursor primitives.
-/
namespace Feed.Emit
open Contract Schema

/-! ## C text helpers -/

def octal (b : Nat) : String :=
  "\\" ++ toString (b / 64) ++ toString (b / 8 % 8) ++ toString (b % 8)

/-- A C string literal with exactly these bytes (UTF-8). -/
def cstr (s : String) : String := Id.run do
  let mut out := "\""
  let mut prevOctal := false
  for b in s.toUTF8.toList do
    let n := b.toNat
    let isDigit := n ≥ 48 && n ≤ 57
    let piece :=
      if n == 34 then "\\\""
      else if n == 92 then "\\\\"
      else if n == 13 then "\\r"
      else if n == 10 then "\\n"
      else if n == 63 then "\\?"
      else if n < 32 || n ≥ 127 then octal n
      else if prevOctal && isDigit then "\"\"" ++ String.singleton (Char.ofNat n)
      else String.singleton (Char.ofNat n)
    prevOctal := n < 32 || n ≥ 127
    out := out ++ piece
  return out ++ "\""

def bytes (s : String) : Nat := s.utf8ByteSize

/-- Fill `@{key}` placeholders. -/
def fill (tpl : String) (kvs : List (String × String)) : String :=
  kvs.foldl (fun acc (k, v) => acc.replace ("@{" ++ k ++ "}") v) tpl

/-! ## Serializers from shapes -/

inductive Piece where
  | lit (s : String) | int (v : String) | str (v : String) | bool (v : String) | embed (p : String)
  deriving Repr, Inhabited

partial def pieces : Shape → List Piece
  | .obj fs =>
    let inner := fs.zipIdx.flatMap fun ((k, v), i) =>
      (if i == 0 then [] else [Piece.lit ","]) ++ [Piece.lit (jsonString k ++ ":")] ++ pieces v
    [Piece.lit "{"] ++ inner ++ [Piece.lit "}"]
  | .int v => [.int v]
  | .str v => [.str v]
  | .bool v => [.bool v]
  | .raw j => [.lit j]
  | .embed p => [.embed p]

/-- Adjacent constant pieces become one literal. -/
def merge (ps : List Piece) : List Piece :=
  (ps.foldl (fun acc p => match acc, p with
    | .lit a :: rest, .lit b => .lit (a ++ b) :: rest
    | _, _ => p :: acc) []).reverse

def outLit (s : String) : String := s!"out_lit(o, {cstr s}, {bytes s});"

def emitPiece : Piece → String
  | .lit s => outLit s
  | .int v => s!"out_i64(o, {v});"
  | .str v => s!"out_jstr(o, {v}, {v}_len);"
  | .bool v => s!"if ({v}) " ++ outLit "true" ++ " else " ++ outLit "false"
  | .embed _ => ""

/-- Upper bound on the bytes a list of pieces writes. -/
def room (ps : List Piece) : String :=
  let fixed := ps.foldl (fun n p => match p with
    | .lit s => n + bytes s | .int _ => n + 20 | .bool _ => n + 5 | _ => n) 0
  let strs := ps.filterMap fun p => match p with | .str v => some s!"JSTR_ROOM({v}_len)" | _ => none
  " + ".intercalate (toString fixed :: strs)

/-- Code for the pieces of a shape before and after its embedded part. -/
def emitAround (s : Shape) : String × String :=
  let ps := merge (pieces s)
  let before := ps.takeWhile (fun p => !(p matches .embed _))
  let after := (ps.dropWhile (fun p => !(p matches .embed _))).drop 1
  let code (xs : List Piece) := s!"out_need(o, {room xs});\n  " ++ "\n  ".intercalate (xs.map emitPiece)
  (code before, code after)

def emitWhole (s : Shape) : String := (emitAround s).1

/-! ## Constant responses -/

def errIdent (e : ErrorCase) : String := "E_" ++ e.name

def emitErrors : String :=
  "\n".intercalate (errors.map fun e =>
    let r := errorResponse e
    s!"static const u8 {errIdent e}[] = {cstr r};  /* {e.status.code} {e.message} */") ++
  "\n#define ERR(name) obuf_static(E_##name, sizeof E_##name - 1)\n"

def hdrIdent (s : Status) : String := s!"HDR_{s.code}"
def emitHeaders : String :=
  "\n".intercalate ([Status.ok, .created, .unavailable].map fun s =>
    s!"static const u8 {hdrIdent s}[] = {cstr (headerPrefix s)};") ++ "\n"
def finish (s : Status) : String := s!"out_finish(&o, {hdrIdent s}, sizeof {hdrIdent s} - 1)"

/-! ## Schema bindings -/

def recordNeeds (t : Table) : Nat := t.cols.length

/-- The record type check of a column, by its schema type. -/
def colCheck (t : Table) (c : String) : String :=
  let k := t.ordinal c
  match (t.column c).ty with
  | .integer => s!"col_is_int(&c[{k}])"
  | .text => s!"col_is_text(&c[{k}])"

def col (t : Table) (c : String) : String := s!"c[{t.ordinal c}]"

/-- Startup check that the stored tables have exactly the derived layout. -/
def emitSchemaCheck : String :=
  let tableCheck (t : Table) :=
    let expect := ",".intercalate (t.cols.map fun c => s!"{c.name}:{c.ty.sql}:{if c.rowid then 1 else 0}")
    s!"  ok &= table_layout({cstr t.name}, {cstr expect});"
  "static int schema_ok(void) {\n  int ok = 1;\n" ++ "\n".intercalate (tables.map tableCheck) ++
  s!"\n  ok &= index_layout({cstr likesByPost.name}, {cstr (",".intercalate likesByPost.keys)});\n  return ok;\n}\n"

/-! ## SQL -/

def sqlFeed : String :=
  s!"SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) " ++
  s!"FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT {feedSize}"
def sqlLike : String :=
  let k := ", ".intercalate likesKey
  s!"INSERT INTO likes ({k}) VALUES (?1, ?2) ON CONFLICT ({k}) DO NOTHING"
def sqlCreate : String := "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at"

/-! ## Routes -/

def methodC : Method → String
  | .get => "M_GET"
  | .post => "M_POST"

def emitSeg : Seg → String
  | .lit s => let t := "/" ++ s; s!"path_lit(&s, e, {cstr t}, {bytes t})"
  | .id => "path_param(&s, e, &id_p, &id_n)"

def emitCheck : Check → String
  | .auth => "if (!authenticate(rq, j)) return;"
  | .postId => "{ int ok = parse_id(id_p, id_n, &j->id); if (ok == 0) { j->resp = ERR(" ++ badId.name ++
      "); return; } if (ok < 0) { j->resp = ERR(" ++ noPost.name ++ "); return; } }"
  | .createBody => "if (!decode_create(rq, j)) return;"

def emitRoute (r : Route) : String :=
  let cond := " && ".intercalate (r.path.map emitSeg ++ ["s == e"])
  let checks := "\n      ".intercalate (r.checks.map emitCheck)
  s!"  if (rq->method == {methodC r.method}) \{
    const u8 *s = p, *e = p + n, *id_p = 0; u32 id_n = 0;
    (void)id_p; (void)id_n;
    if ({cond}) \{
      {checks}
      j->kind = {r.endpoint.kind}; j->is_write = {if r.endpoint.isWrite then 1 else 0};
      return;
    }
  }"

def emitDecode : String :=
  "static void app_decode(const req_t *rq, job_t *j) {\n  const u8 *p = rq->path;\n  u32 n = rq->path_len;\n" ++
  "\n".intercalate (routes.map emitRoute) ++ s!"\n  j->resp = ERR({notFound.name});\n}\n"

/-! ## The file -/

def postArgs : String := "i64 id, const u8 *body, u32 body_len, const u8 *created, u32 created_len, const u8 *author, u32 author_len, i64 likes"

def emitPutPost : String :=
  s!"/* Feed.Contract.postShape */\nstatic inline void put_post(out_t *o, {postArgs}) \{\n  {emitWhole postShape}\n}\n"

def templateMain : String := include_str "templates/app.c"

def appGen : String :=
  let (feedBefore, feedAfter) := emitAround feedShape
  let (postBefore, postAfter) := emitAround onePostShape
  fill templateMain [
    ("ERRORS", emitErrors),
    ("HEADERS", emitHeaders),
    ("SCHEMA_CHECK", emitSchemaCheck),
    ("SQL_FEED", cstr sqlFeed),
    ("SQL_LIKE", cstr sqlLike),
    ("SQL_CREATE", cstr sqlCreate),
    ("PUT_POST", emitPutPost),
    ("DECODE", emitDecode),
    ("FEED_BEFORE", feedBefore),
    ("FEED_AFTER", feedAfter),
    ("POST_BEFORE", postBefore),
    ("POST_AFTER", postAfter),
    ("LIKE_BODY", emitWhole likeShape),
    ("HEALTH_BODY", emitWhole healthShape),
    ("DEGRADED_BODY", emitWhole degradedShape),
    ("CREATE_BEFORE", postBefore),
    ("CREATE_AFTER", postAfter),
    ("FEED_SIZE", toString feedSize),
    ("WINDOW_SPAN", toString (feedSize - 1)),
    ("MAX_BODY_CHARS", toString maxBodyChars),
    ("POSTS_NCOLS", toString (recordNeeds posts)),
    ("USERS_NCOLS", toString (recordNeeds users)),
    ("POSTS_USER", col posts "user_id"),
    ("POSTS_BODY", col posts "body"),
    ("POSTS_CREATED", col posts "created_at"),
    ("POSTS_CHECKS", " && ".intercalate [colCheck posts "user_id", colCheck posts "body", colCheck posts "created_at"]),
    ("POSTS_CREATED_CHECK", colCheck posts "created_at"),
    ("POSTS_USER_CHECK", colCheck posts "user_id"),
    ("USERS_NAME", col users "username"),
    ("USERS_NAME_CHECK", colCheck users "username"),
    ("ERR_NO_POST", noPost.name),
    ("ERR_INTERNAL", internal.name),
    ("ERR_NO_BEARER", noBearer.name),
    ("ERR_BAD_TOKEN", badToken.name),
    ("ERR_BAD_PAYLOAD", badPayload.name),
    ("ERR_BAD_JSON", badJson.name),
    ("ERR_NO_BODY", noBody.name),
    ("ERR_LONG_BODY", longBody.name),
    ("HDR_OK", hdrIdent .ok), ("HDR_CREATED", hdrIdent .created), ("HDR_UNAVAILABLE", hdrIdent .unavailable),
    ("ROOT_POSTS", cstr posts.name), ("ROOT_USERS", cstr users.name), ("ROOT_LIKES_BY_POST", cstr likesByPost.name),
    ("LIKES_BY_POST_KEYS", toString likesByPost.keys.length)
  ]

end Feed.Emit
