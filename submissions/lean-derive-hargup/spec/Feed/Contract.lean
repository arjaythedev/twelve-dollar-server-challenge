import Feed.Model

/-!
# The wire contract

Routes, statuses, error messages and JSON shapes from `SPEC.md`, as data. The emitter turns
this into C (route matching, constant responses, serializers); the oracle renders expected
responses from the same data and the model.
-/
namespace Feed.Contract

inductive Status where
  | ok | created | badRequest | unauthorized | notFound | internal | unavailable
  deriving Repr, DecidableEq, Inhabited

def Status.line : Status → String
  | .ok => "200 OK"
  | .created => "201 Created"
  | .badRequest => "400 Bad Request"
  | .unauthorized => "401 Unauthorized"
  | .notFound => "404 Not Found"
  | .internal => "500 Internal Server Error"
  | .unavailable => "503 Service Unavailable"

def Status.code : Status → Nat
  | .ok => 200 | .created => 201 | .badRequest => 400 | .unauthorized => 401
  | .notFound => 404 | .internal => 500 | .unavailable => 503

/-- Status line and headers, up to and including `Content-Length: `. -/
def headerPrefix (s : Status) : String :=
  "HTTP/1.1 " ++ s.line ++ "\r\nContent-Type: application/json\r\nContent-Length: "

/-! ## Errors -/

structure ErrorCase where
  name : String          -- C identifier suffix
  status : Status
  message : String
  deriving Repr

def notFound : ErrorCase := ⟨"NOT_FOUND", .notFound, "not found"⟩
def badId : ErrorCase := ⟨"BAD_ID", .badRequest, "invalid post id"⟩
def noPost : ErrorCase := ⟨"NO_POST", .notFound, "post not found"⟩
def noBearer : ErrorCase := ⟨"NO_BEARER", .unauthorized, "missing bearer token"⟩
def badToken : ErrorCase := ⟨"BAD_TOKEN", .unauthorized, "invalid or expired token"⟩
def badPayload : ErrorCase := ⟨"BAD_PAYLOAD", .unauthorized, "invalid token payload"⟩
def badJson : ErrorCase := ⟨"BAD_JSON", .badRequest, "malformed JSON body"⟩
def noBody : ErrorCase := ⟨"NO_BODY", .badRequest, "body is required"⟩
def longBody : ErrorCase := ⟨"LONG_BODY", .badRequest, "body must be at most 500 characters"⟩
def internal : ErrorCase := ⟨"INTERNAL", .internal, "internal server error"⟩

def errors : List ErrorCase :=
  [notFound, badId, noPost, noBearer, badToken, badPayload, badJson, noBody, longBody, internal]

/-- `body` length limit, in characters, after trimming. -/
def maxBodyChars : Nat := 500
/-- Posts per feed page. -/
def feedSize : Nat := 20

/-! ## JSON

`json` renders a string value the way the generated serializer does: `"` and `\` are
escaped, control characters use the short escapes or `\u00XX`, and everything else
(including non-ASCII) is copied. -/

def hexDigit (n : Nat) : Char := "0123456789abcdef".toList.getD n '0'

def escapeChar (c : Char) : String :=
  if c = '"' then "\\\""
  else if c = '\\' then "\\\\"
  else if c = '\x08' then "\\b"
  else if c = '\x0c' then "\\f"
  else if c = '\n' then "\\n"
  else if c = '\r' then "\\r"
  else if c = '\t' then "\\t"
  else if c.toNat < 0x20 then "\\u00" ++ String.singleton (hexDigit (c.toNat / 16)) ++ String.singleton (hexDigit (c.toNat % 16))
  else String.singleton c

def jsonString (s : String) : String := "\"" ++ String.join (s.toList.map escapeChar) ++ "\""

/-- A JSON value whose shape is fixed and whose leaves are filled at run time. -/
inductive Shape where
  | obj (fields : List (String × Shape))
  | int (var : String)          -- a C `i64` expression
  | str (var : String)          -- a C byte string `var` with length `var_len`
  | bool (var : String)         -- a C truth value
  | raw (json : String)         -- constant JSON text
  | embed (part : String)       -- JSON written by the plan that uses the shape
  deriving Repr, Inhabited

/-- Values for rendering a shape in Lean (the oracle). -/
inductive Val where
  | int (n : Int) | str (s : String) | bool (b : Bool) | json (text : String)

partial def render (env : String → Val) : Shape → String
  | .obj fs => "{" ++ ",".intercalate (fs.map fun (k, v) => jsonString k ++ ":" ++ render env v) ++ "}"
  | .int x => match env x with | .int n => toString n | _ => "?"
  | .str x => match env x with | .str s => jsonString s | _ => "?"
  | .bool x => match env x with | .bool b => if b then "true" else "false" | _ => "?"
  | .raw j => j
  | .embed f => match env f with | .json t => t | _ => "?"

/-- The post object: `{"id","body","created_at","author","like_count"}` in this order. -/
def postShape : Shape :=
  .obj [("id", .int "id"), ("body", .str "body"), ("created_at", .str "created"),
        ("author", .str "author"), ("like_count", .int "likes")]

/-- `{"posts":[<post>,…]}`: the plan writes the array. -/
def feedShape : Shape := .obj [("posts", .embed "rows")]
def onePostShape : Shape := .obj [("post", .embed "post")]
def likeShape : Shape := .obj [("liked", .raw "true"), ("already_liked", .bool "already"), ("post_id", .int "id")]
def healthShape : Shape := .obj [("status", .raw "\"ok\""), ("db", .raw "\"ok\""), ("uptime_s", .int "uptime")]
def degradedShape : Shape := .obj [("status", .raw "\"degraded\""), ("db", .raw "\"unreachable\""), ("error", .str "msg")]
def errorBody (e : ErrorCase) : String := "{\"error\":" ++ jsonString e.message ++ "}"

def PostView.render (v : PostView) : String :=
  Contract.render (fun
    | "id" => .int v.id | "body" => .str v.body | "created" => .str v.createdAt
    | "author" => .str v.author | "likes" => .int v.likeCount | _ => .int 0) postShape

/-- A complete constant response. -/
def errorResponse (e : ErrorCase) : String :=
  let body := errorBody e
  headerPrefix e.status ++ toString body.utf8ByteSize ++ "\r\n\r\n" ++ body

/-! ## Routes

A route is a method and a path pattern. On a matching request the checks run in order and
the first failure answers. `SPEC.md`: on authenticated routes auth comes before the id. -/

inductive Method where | get | post
  deriving Repr, DecidableEq

inductive Seg where
  | lit (s : String)
  | id                            -- `:id`
  deriving Repr, DecidableEq

inductive Check where
  | auth                          -- Bearer HS256 JWT → uid, username
  | postId                        -- the `:id` segment is a positive integer
  | createBody                    -- JSON body with a non-empty trimmed `body` string
  deriving Repr, DecidableEq

inductive Endpoint where
  | health | feed | post | create | like
  deriving Repr, DecidableEq

def Endpoint.kind : Endpoint → String
  | .health => "K_HEALTH" | .feed => "K_FEED" | .post => "K_POST" | .create => "K_CREATE" | .like => "K_LIKE"

def Endpoint.isWrite : Endpoint → Bool
  | .create | .like => true
  | _ => false

structure Route where
  method : Method
  path : List Seg
  checks : List Check
  endpoint : Endpoint
  deriving Repr

def routes : List Route := [
  ⟨.get, [.lit "health"], [], .health⟩,
  ⟨.get, [.lit "feed"], [], .feed⟩,
  ⟨.get, [.lit "posts", .id], [.postId], .post⟩,
  ⟨.post, [.lit "posts"], [.auth, .createBody], .create⟩,
  ⟨.post, [.lit "posts", .id, .lit "like"], [.auth, .postId], .like⟩
]

end Feed.Contract
