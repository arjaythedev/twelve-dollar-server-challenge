namespace model;

// Shared domain values. No operating-system or database dependencies.
struct ByteRange { start: i64, end: i64 }
enum Method { Get, Post, Other }
struct Request {
    method: Method, close: bool, path: ByteRange, auth: ByteRange,
    has_auth: bool, body: ByteRange, used: i64,
}
enum Parse { NeedMore, Invalid, Complete { request: Request } }
enum Route {
    Health, Feed, Read { id: i64 }, Create, Like { raw_id: ByteRange },
    Missing, InvalidId,
}
enum Auth { Missing, Invalid, InvalidClaims, Verified { user_id: i64 } }
enum Life { Free, Open, Closing }
enum Rx { Multi, Queued, Reading, End }
enum Tx { Idle, Flight { box: i32, sent: u32 } }
enum Input { Empty, Partial { box: i32 }, Ready { box: i32 }, Held { box: i32 } }
struct Handle { index: u32, generation: u32 }
struct Connection {
    generation: u32, life: Life, rx: Rx, tx: Tx, input: Input,
    out: i32, rollback_at: u32, flags: u8,
}

let MAX_HEAD: i64 = 16384;
let MAX_BODY: i64 = 1048576;
let MAX_RINGS: i64 = 16;
let ANSWERS: i64 = 2048;
let READS: i64 = 16384;
let RX_BUFFERS: i64 = 1024;
// These flags describe independent facts, not mutually exclusive lifecycle states.
let CLOSE_AFTER: u8 = 1;
let TOUCHED: u8 = 2;
let ANSWERED: u8 = 4;
let PEER_EOF: u8 = 8;

fn positive_int(s: const u8[:]) -> i64 {
    if s.len == 0 || s.len > 18 || s[0] == '0' { return 0; }
    var n = 0;
    for c in s {
        if c < '0' || c > '9' { return 0; }
        n = n * 10 + c - '0';
    }
    return n;
}
