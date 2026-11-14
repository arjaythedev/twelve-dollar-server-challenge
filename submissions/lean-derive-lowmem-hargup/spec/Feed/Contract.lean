import Feed.Model
import Feed.Wire

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

open Wire

/-- Every dynamic leaf is a typed reference into this shape's finite context. -/
inductive Shape (ctx : List Binding) where
  | obj (fields : List (String × Shape ctx))
  | int (var : Ref ctx .int)
  | str (var : Ref ctx .text)
  | plainStr (var : Ref ctx .plainText)
  | bool (var : Ref ctx .bool)
  | raw (json : String)
  | embed (part : Ref ctx .json)

partial def render (env : Env ctx) : Shape ctx → String
  | .obj fs => "{" ++ ",".intercalate (fs.map fun (k, v) => jsonString k ++ ":" ++ render env v) ++ "}"
  | .int x => toString (x.get env).toInt
  | .str x => jsonString (x.get env)
  | .plainStr x => "\"" ++ (x.get env).value ++ "\""
  | .bool x => if x.get env = true then "true" else "false"
  | .raw j => j
  | .embed f => f.get env

def postCtxFor (bodyTy : Wire.Ty) : List Binding :=
  [⟨"id", .int⟩, ⟨"body", bodyTy⟩, ⟨"created", .text⟩, ⟨"author", .text⟩, ⟨"likes", .int⟩]
def postShapeWith {t : Wire.Ty} (bodyShape : Ref (postCtxFor t) t → Shape (postCtxFor t)) : Shape (postCtxFor t) :=
  .obj [("id", .int .here), ("body", bodyShape (.there .here)),
        ("created_at", .str (.there (.there .here))),
        ("author", .str (.there (.there (.there .here)))),
        ("like_count", .int (.there (.there (.there (.there .here)))))]
def postCtx := postCtxFor .text
def postShape : Shape postCtx := postShapeWith Shape.str
def postPlainShape : Shape (postCtxFor .plainText) := postShapeWith Shape.plainStr

def feedShape : Shape [⟨"rows", .json⟩] := .obj [("posts", .embed .here)]
def onePostShape : Shape [⟨"post", .json⟩] := .obj [("post", .embed .here)]
def likeShape : Shape [⟨"already", .bool⟩, ⟨"id", .int⟩] :=
  .obj [("liked", .raw "true"), ("already_liked", .bool .here), ("post_id", .int (.there .here))]
def healthShape : Shape [⟨"uptime", .int⟩] :=
  .obj [("status", .raw "\"ok\""), ("db", .raw "\"ok\""), ("uptime_s", .int .here)]
def degradedShape : Shape [⟨"msg", .text⟩] :=
  .obj [("status", .raw "\"degraded\""), ("db", .raw "\"unreachable\""), ("error", .str .here)]
def errorBody (e : ErrorCase) : String := "{\"error\":" ++ jsonString e.message ++ "}"

/-- The model's natural numbers must fit the C wire domain before rendering. -/
def PostView.render (v : PostView) : Option String :=
  if v.id ≤ 9223372036854775807 ∧ v.likeCount ≤ 9223372036854775807 then
    some (Contract.render (ctx := postCtx) (Int64.ofInt v.id, v.body, v.createdAt, v.author,
      Int64.ofInt v.likeCount, ()) postShape)
  else none

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

/-- Values an endpoint consumes after request validation. -/
inductive Arg where
  | postId | userId | username | body
  deriving Repr, DecidableEq

def Endpoint.payload : Endpoint → List Arg
  | .health | .feed => []
  | .post => [.postId]
  | .like => [.postId, .userId]
  | .create => [.userId, .username, .body]

/-- A field reference cannot be formed for an endpoint that does not carry it. -/
structure PayloadRef (e : Endpoint) (a : Arg) where
  present : a ∈ e.payload

theorem postIdRef : PayloadRef .post .postId := ⟨by decide⟩
theorem likeIdRef : PayloadRef .like .postId := ⟨by decide⟩
theorem likeUserRef : PayloadRef .like .userId := ⟨by decide⟩
theorem createUserRef : PayloadRef .create .userId := ⟨by decide⟩
theorem createNameRef : PayloadRef .create .username := ⟨by decide⟩
theorem createBodyRef : PayloadRef .create .body := ⟨by decide⟩

def Endpoint.needsName (e : Endpoint) : Bool := e.payload.contains .username

theorem Endpoint.needsName_iff (e : Endpoint) : e.needsName = true ↔ e = .create := by
  cases e <;> decide

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
