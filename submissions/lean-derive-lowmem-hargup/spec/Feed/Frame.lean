/-! Framing state has a phase-specific, pointer-free payload. The field descriptor selects
both the concrete C layout and the save/restore operations. C header validation is the trust
boundary for constructing `body`; it may only retain offsets into the complete header. -/
namespace Feed.Frame
inductive Phase where | head | body
  deriving DecidableEq
inductive Field where
  | scanned | method | close | hasAuth | pathOff | pathLen | authOff | authLen | headLen | bodyLen
  deriving DecidableEq

def Phase.fields : Phase → List Field
  | .head => [.scanned]
  | .body => [.method, .close, .hasAuth, .pathOff, .pathLen, .authOff, .authLen, .headLen, .bodyLen]
structure Ref (p : Phase) (f : Field) where
  present : f ∈ p.fields

def Field.name : Field → String
  | .scanned => "scanned" | .method => "method" | .close => "close" | .hasAuth => "has_auth"
  | .pathOff => "path_off" | .pathLen => "path_len" | .authOff => "auth_off"
  | .authLen => "auth_len" | .headLen => "head_len" | .bodyLen => "body_len"
def Field.ctype : Field → String
  | .method | .close | .hasAuth => "u8"
  | _ => "u32"
def Phase.name : Phase → String | .head => "head" | .body => "body"
def access {p : Phase} {f : Field} (_ : Ref p f) : String := s!"fs->{p.name}.{f.name}"

def save (f : Field) : String := match f with
  | .pathOff => "(u32)(r->path - p)"
  | .authOff => "r->has_auth ? (u32)(r->auth - p) : 0"
  | .headLen => "he"
  | .bodyLen => "clen"
  | _ => "r->" ++ f.name

def restore (f : Field) : String := match f with
  | .pathOff => "r->path = p + fs->body.path_off;"
  | .authOff => "r->auth = fs->body.has_auth ? p + fs->body.auth_off : 0;"
  | .headLen => "r->body = p + fs->body.head_len;"
  | _ => s!"r->{f.name} = fs->body.{f.name};"

def header : String :=
  "/* GENERATED phase-specific framing storage. No borrowed receive pointers. */\n" ++
  "enum { FRAME_HEAD, FRAME_BODY };\ntypedef struct {\n  u32 phase;\n  union {\n" ++
  "\n".intercalate ([Phase.head, .body].map fun p =>
    "    struct { " ++ " ".intercalate (p.fields.map fun f => s!"{f.ctype} {f.name};") ++ s!" } {p.name};") ++
  "\n  };\n} frame_state;\n_Static_assert(sizeof(frame_state) == 32, \"cold frame layout\");\n" ++
  "static void frame_save_body(frame_state *fs, const req_t *r, const u8 *p, u32 he, u32 clen) {\n" ++
  "\n".intercalate (Phase.body.fields.attach.map fun ⟨f, h⟩ => s!"  {access (⟨h⟩ : Ref .body f)} = {save f};") ++
  "\n  fs->phase = FRAME_BODY;\n}\n" ++
  "static void frame_restore_body(const frame_state *fs, req_t *r, const u8 *p) {\n" ++
  "\n".intercalate (Phase.body.fields.map fun f => "  " ++ restore f) ++ "\n}\n"
end Feed.Frame
