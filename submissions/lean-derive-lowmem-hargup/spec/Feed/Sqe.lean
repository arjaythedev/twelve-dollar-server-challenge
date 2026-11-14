import Feed.Wire

/-! SQE constructors are specialized by opcode. Dynamic assignments are typed references
into that opcode's exact argument context. A plan must initialize every required dynamic
field; zeroing plus opcode constants supplies the remaining ABI fields. The Linux ABI and
C emission are trusted boundaries, not claims of a machine-checked kernel proof. -/
namespace Feed.Sqe
open Wire
inductive Op where | accept | recv | send | shutdown | close | cancel
  deriving DecidableEq

def Op.name : Op → String
  | .accept => "accept" | .recv => "recv" | .send => "send"
  | .shutdown => "shutdown" | .close => "close" | .cancel => "cancel"
def Op.ctx : Op → List Binding
  | .accept | .recv | .shutdown => [⟨"fd", .fd⟩, ⟨"tag", .u64⟩]
  | .send => [⟨"fd", .fd⟩, ⟨"addr", .u64⟩, ⟨"len", .u32⟩, ⟨"tag", .u64⟩]
  | .close => [⟨"index", .u32⟩, ⟨"tag", .u64⟩]
  | .cancel => [⟨"target", .u64⟩, ⟨"tag", .u64⟩]
inductive Field : Wire.Ty → Type where
  | fd : Field .fd
  | addr : Field .u64
  | len : Field .u32
  | fileIndex : Field .u32
  | tag : Field .u64
def Field.name : Field t → String
  | .fd => "fd" | .addr => "addr" | .len => "len" | .fileIndex => "file_index" | .tag => "user_data"
structure Assignment (ctx : List Binding) where
  ty : Wire.Ty
  field : Field ty
  value : Ref ctx ty

def assign (f : Field t) (v : Ref ctx t) : Assignment ctx := ⟨t, f, v⟩
def Op.required : Op → List String
  | .accept | .recv | .shutdown => ["fd", "user_data"]
  | .send => ["fd", "addr", "len", "user_data"]
  | .close => ["file_index", "user_data"]
  | .cancel => ["addr", "user_data"]
structure ReadyPlan (op : Op) where
  assignments : List (Assignment op.ctx)
  complete : assignments.map (fun a => a.field.name) = op.required

def plan : (op : Op) → ReadyPlan op
  | .accept => ⟨[assign .fd .here, assign .tag (.there .here)], rfl⟩
  | .recv => ⟨[assign .fd .here, assign .tag (.there .here)], rfl⟩
  | .shutdown => ⟨[assign .fd .here, assign .tag (.there .here)], rfl⟩
  | .send => ⟨[assign .fd .here, assign .addr (.there .here),
      assign .len (.there (.there .here)), assign .tag (.there (.there (.there .here)))], rfl⟩
  | .close => ⟨[assign .fileIndex .here, assign .tag (.there .here)], rfl⟩
  | .cancel => ⟨[assign .addr .here, assign .tag (.there .here)], rfl⟩

def Op.constants : Op → String
  | .accept => "s->opcode = IORING_OP_ACCEPT; s->ioprio = IORING_ACCEPT_MULTISHOT; s->file_index = IORING_FILE_INDEX_ALLOC;"
  | .recv => "s->opcode = IORING_OP_RECV; s->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT; s->ioprio = IORING_RECV_MULTISHOT; s->buf_group = 0;"
  | .send => "s->opcode = IORING_OP_SEND; s->flags = IOSQE_FIXED_FILE; s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;"
  | .shutdown => "s->opcode = IORING_OP_SHUTDOWN; s->flags = IOSQE_FIXED_FILE | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS; s->len = SHUT_RDWR;"
  | .close => "s->opcode = IORING_OP_CLOSE;"
  | .cancel => "s->opcode = IORING_OP_ASYNC_CANCEL; s->flags = IOSQE_CQE_SKIP_SUCCESS;"

def argDecl (b : Binding) : String :=
  (match b.ty with | .fd => "int " | .u32 => "u32 " | .u64 => "u64 " | _ => "UNSUPPORTED ") ++ b.name
/-- Only a complete plan may emit the transition from reserved storage to a ready entry. -/
def emit (p : ReadyPlan op) : String :=
  s!"static void sqe_{op.name}(ring *r, {", ".intercalate (op.ctx.map argDecl)}) \{\n" ++
  "  sqe_reserved slot = sqe_reserve(r);\n  struct io_uring_sqe *s = slot.p;\n  " ++ op.constants ++ "\n" ++
  "\n".intercalate (p.assignments.map fun a => s!"  s->{a.field.name} = {a.value.name};") ++
  "\n  sqe_commit(r, (sqe_ready){s});\n}\n"
def header : String :=
  "/* GENERATED opcode-specific constructors; zero, initialize, then commit privately. */\n" ++
  "\n".intercalate ([Op.accept, .recv, .send, .shutdown, .close, .cancel].map fun op => emit (plan op))
end Feed.Sqe
