// MIT. Codec routines adapted from goose-iouring-ahmadsh2007 and sstadick PR 1.
namespace record;

fn varint_at(p: const u8[:], i: i64) -> u64, i64 {
    if i < p.len && p[i] < 0x80 { return p[i], i + 1; }
    var v: u64 = 0;
    var k = i;
    for n in 9 {
        if k >= p.len { return 0, -1; }
        let b = p[k];
        k++;
        if n == 8 { return (v << 8) | b, k; }
        v = (v << 7) | (b & 0x7f);
        if b < 0x80 { return v, k; }
    }
    return v, k;
}

fn serial_len(t: u64) -> i64 {
    if t >= 12 { return ((t - 12) / 2) as i64; }
    return match t { 1 => 1, 2 => 2, 3 => 3, 4 => 4, 5 => 6, 6 => 8, 7 => 8, _ => 0 };
}

struct Cols { t: u64[4], s: i64[4], e: i64[4] }

inline fn columns(rec: const u8[:], s: i64, e: i64, n: i64, c: Cols&) -> bool {
    assert(n >= 0);
    assert(n <= 4);
    if s < 0 || e < s || e > rec.len { return false; }
    let hsize, i0 = varint_at(rec[..e], s);
    if i0 < 0 || hsize > (e - s) as u64 { return false; }
    let hend = s + hsize as i64;
    if hsize as i64 > e - s { return false; }
    var i = i0;
    var at = hend;
    for k in n {
        if i >= hend { return false; }
        let t, ni = varint_at(rec[..hend], i);
        if ni < 0 { return false; }
        i = ni;
        let l = serial_len(t);
        if at + l > e { return false; }
        c.t[k] = t;
        c.s[k] = at;
        c.e[k] = at + l;
        at += l;
    }
    return true;
}

fn is_text(t: u64) -> bool { return t >= 13 && (t & 1) == 1; }

fn int_col(rec: const u8[:], c: Cols&, k: i64) -> i64, bool {
    let t = c.t[k];
    if t == 8 { return 0, true; }
    if t == 9 { return 1, true; }
    if t < 1 || t > 6 { return 0, false; }
    let s = c.s[k];
    let e = c.e[k];
    var v = rec[s] as i8 as i64;
    for i in s + 1..e { v = (v << 8) | rec[i]; }
    return v, true;
}
