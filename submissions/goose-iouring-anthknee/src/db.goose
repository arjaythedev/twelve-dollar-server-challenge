// SQLite read plan adapted from rust-iouring-kensac.
import src.native;
import src.record;
import src.json;
import src.model;
namespace db;

enum Lookup { Found, Missing, Failed }
enum Like { Created, Existing, Missing, Failed }
enum Plan { Ready, Fallback, Failed }
enum Checkpoint { Idle, Active { started: i64 } }

let POSTS: i32 = 0;
let USERS: i32 = 1;
let FEED: i32 = 2;
let LIKES: i32 = 3;
struct Row {
    id: i64, body: model::ByteRange, created: model::ByteRange,
    author: model::ByteRange, likes: i64,
}
var rows: Row[20];
var order: i64[20];
var row_count: i64 = 0;
var counts: u32[513];
var text: u8[>..];
var checkpoint: Checkpoint = Checkpoint.Idle;

fn count_slow(id: i64) -> i64 {
    var cursor = native::seek_ge(LIKES, id);
    var count = 0;
    var bytes: u8[>..] = [];
    var cols = record::Cols { .. };
    while cursor == 1 {
        bytes.clear();
        let size = native::row(LIKES, bytes);
        if size < 0 || !record::columns(bytes, 0, size, 1, cols) { return -1; }
        let value, ok = record::int_col(bytes, cols, 0);
        if !ok { return -1; }
        if value != id { break; }
        count++;
        cursor = native::next(LIKES);
    }
    return if cursor < 0 { -1 } else { count };
}

fn count_likes(id: i64) -> i64 {
    counts[0] = 0;
    let result = native::like_counts(id, id, counts[..1]);
    if result == 0 { return counts[0]; }
    if result < 0 { return -1; }
    return count_slow(id);
}

fn author_name(start: i64, end: i64, span: model::ByteRange&) -> Lookup {
    // SQLite stores the INTEGER PRIMARY KEY alias as NULL in the record.
    if start >= 0 && end <= text.len && end - start >= 3 {
        let header = text[start] as i64;
        let kind = text[start + 2] as i64;
        if header >= 3 && header < 128 && header <= end - start &&
           text[start + 1] == 0 && kind >= 13 && kind < 128 && (kind & 1) == 1 {
            let first = start + header;
            let len = (kind - 13) / 2;
            if len <= end - first {
                span = model::ByteRange { start: first, end: first + len };
                return Lookup.Found;
            }
        }
    }
    var cols = record::Cols { .. };
    if !record::columns(text, start, end, 2, cols) || !record::is_text(cols.t[1]) { return Lookup.Failed; }
    span = model::ByteRange { start: cols.s[1], end: cols.e[1] };
    return Lookup.Found;
}

fn author(uid: i64, span: model::ByteRange&) -> Lookup {
    let found = native::seek(USERS, uid);
    if found < 0 { return Lookup.Failed; }
    if found == 0 { return Lookup.Missing; }
    let start = text.len;
    let size = native::row(USERS, text);
    if size < 0 { return Lookup.Failed; }
    return author_name(start, start + size, span);
}

fn post_json(out: u8[>..]&, id: i64, body: const u8[:], created: const u8[:],
             author: const u8[:], likes: i64) {
    out.append("{\"id\":"); json::push_int(out, id);
    out.append(",\"body\":\""); json::esc(out, body);
    out.append("\",\"created_at\":\""); json::esc(out, created);
    out.append("\",\"author\":\""); json::esc(out, author);
    out.append("\",\"like_count\":"); json::push_int(out, likes);
    out.push('}');
}

fn post(id: i64, out: u8[>..]&) -> Lookup {
    assert(id > 0);
    if native::begin() != 0 { return Lookup.Failed; }
    let found = native::seek(POSTS, id);
    if found < 0 { return Lookup.Failed; }
    if found == 0 { return Lookup.Missing; }
    text.clear();
    let size = native::row(POSTS, text);
    var cols = record::Cols { .. };
    if size < 0 || !record::columns(text, 0, size, 4, cols) { return Lookup.Failed; }
    if !record::is_text(cols.t[2]) || !record::is_text(cols.t[3]) { return Lookup.Failed; }
    let uid, valid = record::int_col(text, cols, 1);
    if !valid { return Lookup.Failed; }
    var name = model::ByteRange { start: 0, end: 0 };
    let lookup = author(uid, name);
    match lookup {
        Missing => { return Lookup.Missing; },
        Failed => { return Lookup.Failed; },
        Found => {},
    }
    let likes = count_likes(id);
    if likes < 0 { return Lookup.Failed; }
    out.append("{\"post\":");
    post_json(out, id, text[cols.s[2]..cols.e[2]], text[cols.s[3]..cols.e[3]],
              text[name.start..name.end], likes);
    out.push('}');
    return Lookup.Found;
}

fn feed_rows() -> Plan {
    text.clear();
    row_count = 0;
    var cols = record::Cols { .. };
    var cursor = native::first(FEED);
    while cursor == 1 && row_count < 20 {
        let start = text.len;
        let size = native::row(FEED, text);
        if size < 0 || !record::columns(text, start, start + size, 2, cols) { return Plan.Failed; }
        if !record::is_text(cols.t[0]) { return Plan.Failed; }
        let id, valid = record::int_col(text, cols, 1);
        if !valid { return Plan.Failed; }
        rows[row_count] = Row { id: id,
            body: model::ByteRange { start: 0, end: 0 },
            created: model::ByteRange { start: cols.s[0], end: cols.e[0] },
            author: model::ByteRange { start: 0, end: 0 }, likes: 0 };
        row_count++;
        cursor = native::next(FEED);
    }
    if cursor < 0 { return Plan.Failed; }
    if row_count == 0 { return Plan.Ready; }
    assert(row_count <= 20);
    for i in row_count { order[i] = row_count - 1 - i; }
    for i in 1..row_count {
        let value = order[i];
        var j = i;
        while j > 0 && rows[order[j - 1]].id > rows[value].id {
            order[j] = order[j - 1]; j--;
        }
        order[j] = value;
    }
    var author_ids: i64[20];
    for k in row_count {
        let i = order[k];
        let found = native::seek(POSTS, rows[i].id);
        if found < 0 { return Plan.Failed; }
        if found == 0 { return Plan.Fallback; }
        let start = text.len;
        let size = native::row(POSTS, text);
        if size < 0 || !record::columns(text, start, start + size, 3, cols) { return Plan.Failed; }
        if !record::is_text(cols.t[2]) { return Plan.Failed; }
        let uid, valid = record::int_col(text, cols, 1);
        if !valid { return Plan.Failed; }
        author_ids[i] = uid;
        rows[i].body = model::ByteRange { start: cols.s[2], end: cols.e[2] };
    }
    var offsets: i64[21];
    let gathered = native::feed_author_names(author_ids[..row_count], offsets[..row_count + 1], text);
    for i in row_count {
        if gathered {
            rows[i].author = model::ByteRange { start: offsets[i], end: offsets[i + 1] };
            continue;
        }
        let found = author(author_ids[i], rows[i].author);
        match found {
            Missing => { return Plan.Fallback; },
            Failed => { return Plan.Failed; },
            Found => {},
        }
    }
    let low = rows[order[0]].id;
    let high = rows[order[row_count - 1]].id;
    if high - low <= 512 {
        let span = high - low + 1;
        for i in span { counts[i] = 0; }
        let result = native::like_counts(low, high, counts[..span]);
        if result < 0 { return Plan.Failed; }
        if result == 0 {
            for i in row_count { rows[i].likes = counts[rows[i].id - low]; }
            return Plan.Ready;
        }
    }
    for i in row_count {
        let count = count_likes(rows[i].id);
        if count < 0 { return Plan.Failed; }
        rows[i].likes = count;
    }
    return Plan.Ready;
}

fn feed_sql(out: u8[>..]&) -> bool {
    var row = native::FeedRow { .. };
    // A full 20-row feed needs a final step to reach SQLITE_DONE.
    for i in 21 {
        text.clear();
        let result = native::feed_step(row, text);
        if result < 0 { return false; }
        if result == 0 { return true; }
        if i == 20 { return false; }
        if i > 0 { out.push(','); }
        let c = row.body_len;
        let a = c + row.created_len;
        post_json(out, row.id, text[..c], text[c..a], text[a..a + row.author_len], row.likes);
    }
    return false;
}

fn feed(out: u8[>..]&) -> bool {
    if native::begin() != 0 { return false; }
    let plan = feed_rows();
    out.append("{\"posts\":[");
    match plan {
        Failed => { return false; },
        Fallback => { if !feed_sql(out) { return false; } },
        Ready => {
            for i in row_count {
                let row = rows[i];
                if i > 0 { out.push(','); }
                post_json(out, row.id, text[row.body.start..row.body.end],
                    text[row.created.start..row.created.end], text[row.author.start..row.author.end], row.likes);
            }
        },
    }
    out.append("]}");
    return true;
}

fn like(uid: i64, id: i64) -> Like {
    assert(uid > 0); assert(id > 0);
    if native::begin() != 0 { return Like.Failed; }
    let found = native::seek(POSTS, id);
    if found < 0 { return Like.Failed; }
    if found == 0 { return Like.Missing; }
    let result = native::add_like(uid, id);
    if result < 0 { return Like.Failed; }
    return if result == 1 { Like.Created } else { Like.Existing };
}

fn writes_held() -> bool {
    return match checkpoint { Idle => false, Active => true };
}

fn tick(now: i64) {
    let status = native::checkpoint_poll();
    let frames = native::wal_frames();
    match checkpoint {
        Idle => {
            if frames >= 4096 {
                checkpoint = Checkpoint.Active { started: now };
                native::checkpoint_request();
            }
        },
        Active a => {
            if status >= 2 {
                if status == 2 { native::wal_reset(); }
                if status != 2 && frames >= 65536 {
                    checkpoint = Checkpoint.Active { started: now };
                    native::checkpoint_request();
                } else {
                    checkpoint = Checkpoint.Idle;
                }
            } else if frames < 65536 && now - a.started >= 250000 {
                native::checkpoint_interrupt();
            }
        },
    }
}
