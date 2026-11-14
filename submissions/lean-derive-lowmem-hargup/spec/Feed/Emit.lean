import Feed.Contract
import Feed.Bounds
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
open Contract Schema Wire

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
  | lit (s : String) | int (v : String) | str (v : String) | plainStr (v : String) | bool (v : String) | embed (p : String)
  deriving Repr, Inhabited

partial def pieces : Shape ctx → List Piece
  | .obj fs =>
    let inner := fs.zipIdx.flatMap fun ((k, v), i) =>
      (if i == 0 then [] else [Piece.lit ","]) ++ [Piece.lit (jsonString k ++ ":")] ++ pieces v
    [Piece.lit "{"] ++ inner ++ [Piece.lit "}"]
  | .int v => [.int v.name]
  | .str v => [.str v.name]
  | .plainStr v => [.plainStr v.name]
  | .bool v => [.bool v.name]
  | .raw j => [.lit j]
  | .embed p => [.embed p.name]

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
  | .plainStr v => s!"out_plain_jstr(o, {v}, {v}_len);"
  | .bool v => s!"if ({v}) " ++ outLit "true" ++ " else " ++ outLit "false"
  | .embed _ => ""

/-- Upper bound on the bytes a list of pieces writes. -/
def room (ps : List Piece) : String :=
  let fixed := ps.foldl (fun n p => match p with
    | .lit s => n + bytes s | .int _ => n + 20 | .bool _ => n + 5 | _ => n) 0
  let strs := ps.filterMap fun p => match p with | .str v => some s!"JSTR_ROOM({v}_len)" | .plainStr v => some s!"({v}_len + 2)" | _ => none
  " + ".intercalate (toString fixed :: strs)

/-- Code for the pieces of a shape before and after its embedded part. -/
def emitAround (s : Shape ctx) : String × String :=
  let ps := merge (pieces s)
  let before := ps.takeWhile (fun p => !(p matches .embed _))
  let after := (ps.dropWhile (fun p => !(p matches .embed _))).drop 1
  let code (xs : List Piece) := s!"out_need(o, {room xs});\n  " ++ "\n  ".intercalate (xs.map emitPiece)
  (code before, code after)

def emitWhole (s : Shape ctx) : String := (emitAround s).1

/-- The proof-bearing shape selects inline ownership; C assertions tie its arithmetic
constants to the runtime's actual allocation/header sizes. -/
def boundedBegin (b : Bounded s inlineCapacity) : String :=
  s!"_Static_assert(OUT_INLINE == {inlineCapacity} && HDR_ROOM == 96, \"wire capacity contract\");\n  " ++
  s!"_Static_assert(HDR_ROOM + {b.body} <= OUT_INLINE, \"bounded response fits\");\n  out_begin_inline(o);"

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
def colCheck {t : Table} {ty : Schema.Ty} (f : Field t ty) : String :=
  let k := f.index.val
  match ty with
  | .integer => s!"col_is_int(&c[{k}])"
  | .text => s!"col_is_text(&c[{k}])"

def col (f : Field t ty) : String := s!"c[{f.index.val}]"

/-- The field's membership proof determines the exact required prefix and array size. -/
def textProjection (f : Field t .text) : String :=
  s!"col_t c[{f.prefix}];\n  if (rec_cols(&r, {f.prefix}, c) || !col_is_text(&c[{f.index.val}])) return -1;\n  *author = c[{f.index.val}].p; *author_len = col_len(&c[{f.index.val}]);"

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
/-- The post object for one id, as in `SPEC.md`. -/
def sqlPost : String :=
  s!"SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) " ++
  s!"FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?1"
def sqlPostExists : String := "SELECT 1 FROM posts WHERE id = ?1"
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

def payloadName : Endpoint → String
  | .health => "health" | .feed => "feed" | .post => "post" | .like => "like" | .create => "create"
def argName : Arg → String
  | .postId => "id" | .userId => "uid" | .username => "name" | .body => "text"
def argDecl (a : Arg) : String := match a with
  | .postId | .userId => s!"i64 {argName a};"
  | .username => s!"const u8 *{argName a}; u32 {argName a}_len;"
  | .body => "const u8 *text; u32 text_len; u8 text_plain;"
def payloadAccess {e : Endpoint} {a : Arg} (_ : PayloadRef e a) : String :=
  s!"((args_{payloadName e} *)j->payload)->{argName a}"

def jobGen : String :=
  "/* GENERATED from Endpoint.payload. */\n" ++
  "\n".intercalate ([Endpoint.post, .like, .create].map fun e =>
    "typedef struct { " ++ " ".intercalate (e.payload.map argDecl) ++ s!" } args_{payloadName e};") ++
  "\ntypedef struct job {\n  u8 kind, is_write, close_after;\n  u32 conn, gen, next;\n  void *payload;\n  obuf *resp;\n} job_t;\n" ++
  "_Static_assert(sizeof(job_t) == 32, \"compact job envelope\");\n"

def emitCheck (_e : Endpoint) : Check → String
  | .auth => "if (!authenticate(rq, j, &claims)) return;"
  | .postId => "{ int ok = parse_id(id_p, id_n, &id); if (ok == 0) { j->resp = ERR(" ++ badId.name ++
      "); return; } if (ok < 0) { j->resp = ERR(" ++ noPost.name ++ "); return; } }"
  | .createBody => "if (!decode_create(rq, j, &text, &text_len, &text_plain)) return;"

def constructArg : Arg → String
  | .postId => "a->id = id;"
  | .userId => "a->uid = claims.uid;"
  | .body => "a->text = text; a->text_len = text_len; a->text_plain = text_plain;"
  | .username => "u8 *name = arena_get(claims.raw_name_len); if (!name) { j->resp = ERR(INTERNAL); return; } a->name_len = claims.name_escaped ? json_unescape(claims.raw_name, claims.raw_name_len, name) : (memcpy(name, claims.raw_name, claims.raw_name_len), claims.raw_name_len); a->name = name;"

def emitRoute (r : Route) : String :=
  let cond := " && ".intercalate (r.path.map emitSeg ++ ["s == e"])
  let checks := "\n      ".intercalate (r.checks.map (emitCheck r.endpoint))
  let args := r.endpoint.payload
  let declarations := (if args.contains .postId then "i64 id; " else "") ++
    (if args.contains .userId then "claims_t claims; " else "") ++
    (if args.contains .body then "const u8 *text; u32 text_len; u8 text_plain; " else "")
  let constructor := if args.isEmpty then "" else
    s!"args_{payloadName r.endpoint} *a = (void *)arena_get(sizeof *a); if (!a) \{ j->resp = ERR(INTERNAL); return; } j->payload = a;\n      " ++
    "\n      ".intercalate (args.map constructArg)
  let stream := if r.endpoint.isWrite then "writes" else payloadName r.endpoint
  s!"  if (rq->method == {methodC r.method}) \{
    const u8 *s = p, *e = p + n, *id_p = 0; u32 id_n = 0;
    (void)id_p; (void)id_n;
    if ({cond}) \{
      {declarations}
      {checks}
      {constructor}
      j->kind = {r.endpoint.kind}; j->is_write = {if r.endpoint.isWrite then 1 else 0};
      stream_append(&stream_{stream}, j, slot);
      return;
    }
  }"

def emitDecode : String :=
  "static void app_decode(const req_t *rq, job_t *j, u32 slot) {\n  const u8 *p = rq->path;\n  u32 n = rq->path_len;\n" ++
  "\n".intercalate (routes.map emitRoute) ++ s!"\n  j->resp = ERR({notFound.name});\n}\n"

/-! ## The file -/

def cArg (b : Binding) : String :=
  match b.ty with
  | .int => s!"i64 {b.name}"
  | .fd => s!"int {b.name}"
  | .u32 => s!"u32 {b.name}"
  | .u64 => s!"u64 {b.name}"
  | .bool => s!"int {b.name}"
  | .text | .plainText | .json => s!"const u8 *{b.name}, u32 {b.name}_len"
def postArgs : String := ", ".intercalate (postCtx.map cArg)

def emitPutPost : String :=
  s!"/* Feed.Contract.postShape */\nstatic inline void put_post(out_t *o, {postArgs}) \{\n  {emitWhole postShape}\n}\n"

/-- Initial send-buffer class: a shape that embeds rows (a list) starts in a big block, a single
object in a small one. Either grows if the data needs it. -/
partial def hasRows : Shape ctx → Bool
  | .obj fs => fs.any fun (k, v) => (k == "posts" && v matches .embed _) || hasRows v
  | _ => false
def bufferClass (s : Shape ctx) : String := if hasRows s then "OUT_BIG" else "OUT_SMALL"

def templateMain : String := include_str "templates/app.c"

def appGen : String :=
  let (feedBefore, feedAfter) := emitAround feedShape
  let (postBefore, postAfter) := emitAround onePostShape
  fill templateMain [
    ("ERRORS", emitErrors),
    ("HEADERS", emitHeaders),
    ("SCHEMA_CHECK", emitSchemaCheck),
    ("SQL_FEED", cstr sqlFeed),
    ("SQL_POST", cstr sqlPost),
    ("SQL_POST_EXISTS", cstr sqlPostExists),
    ("SQL_LIKE", cstr sqlLike),
    ("SQL_CREATE", cstr sqlCreate),
    ("PUT_POST", emitPutPost ++
      s!"static inline void put_post_plain(out_t *o, {postArgs}) \{\n  {emitWhole postPlainShape}\n}\n"),
    ("DECODE", emitDecode),
    ("POST_ID", payloadAccess postIdRef),
    ("LIKE_ID", payloadAccess likeIdRef),
    ("LIKE_USER", payloadAccess likeUserRef),
    ("CREATE_USER", payloadAccess createUserRef),
    ("CREATE_NAME", payloadAccess createNameRef),
    ("CREATE_BODY", payloadAccess createBodyRef),
    ("FEED_BEFORE", feedBefore),
    ("FEED_AFTER", feedAfter),
    ("POST_BEFORE", postBefore),
    ("POST_AFTER", postAfter),
    ("LIKE_BEGIN", boundedBegin likeBound),
    ("HEALTH_BEGIN", boundedBegin healthBound),
    ("LIKE_BODY", emitWhole likeShape),
    ("HEALTH_BODY", emitWhole healthShape),
    ("DEGRADED_BODY", emitWhole degradedShape),
    ("CREATE_BEFORE", postBefore),
    ("CREATE_AFTER", postAfter),
    ("FEED_SIZE", toString feedSize),
    ("FEED_CLASS", bufferClass feedShape),
    ("OBJECT_CLASS", bufferClass postShape),
    ("WINDOW_SPAN", toString (feedSize - 1)),
    ("MAX_BODY_CHARS", toString maxBodyChars),
    ("POSTS_NCOLS", toString (recordNeeds posts)),
    ("USERS_NAME_PROJECTION", textProjection usersName),
    ("POSTS_USER", col postsUser),
    ("POSTS_BODY", col postsBody),
    ("POSTS_CREATED", col postsCreated),
    ("POSTS_CHECKS", " && ".intercalate [colCheck postsUser, colCheck postsBody, colCheck postsCreated]),
    ("POSTS_CREATED_CHECK", colCheck postsCreated),
    ("POSTS_USER_CHECK", colCheck postsUser),
    ("USERS_NAME", col usersName),
    ("USERS_NAME_CHECK", colCheck usersName),
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
  ] |>.splitOn "\n" |>.map (fun line => line.trimAsciiEnd.toString) |> "\n".intercalate

end Feed.Emit
