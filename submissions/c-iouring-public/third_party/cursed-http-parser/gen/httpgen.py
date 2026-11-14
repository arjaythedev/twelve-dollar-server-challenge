#!/usr/bin/env python3
"""httpgen: generate the API-specific part of fastparse.h from an API spec, optionally profile-guided.

  httpgen.py gen SPEC.json --base include/fastparse.h -o fastparse_gen.h [--profile PROFILE.json]
  httpgen.py profile SPEC.json CAPTURE > PROFILE.json

`gen` copies --base and replaces the region between "==== BEGIN API SPEC ====" and
"==== END API SPEC ====". It emits route ids, the general router, canonical request-line templates
and the policy hooks. It also replaces the small "API SPEC FIELDS" region before req_t: the headers the
spec asks for by name (a route's "headers": ["X-Request-Id", ...]) become req_t.hdr[FP_H_X_REQUEST_ID] and
hdr_len[...] on those routes. The generic SIMD parser around both regions is not touched.

`profile` reads a capture (raw HTTP requests back to back, as recv() would see them) and writes
per-route counts and statistics as JSON.

PGO here means *decisions*, never bytes. `gen` reads only numbers from a profile:
  - the order in which canonical templates are tried (most frequent first);
  - per route, whether the learned-layout fast path is worth attempting (stable layouts only);
  - per method, whether to use the walk core (long header blocks, or auth needed).
No header name, value or other traffic byte reaches the generated C. Every literal in it comes
from the spec (methods, paths) or from HTTP itself ("HTTP/1.1\r\n").
"""
import json, re, statistics, sys
from collections import Counter, defaultdict

DIGITS = set(b"0123456789")
VERSION = b" HTTP/1.1\r\n"


# ---------------- spec model ----------------
# headers the parsers already handle themselves; a spec can't ask for them by name
RESERVED_HEADERS = {"content-length", "transfer-encoding", "connection", "authorization"}


class Route:
    def __init__(self, d, params):
        self.name, self.method, self.path = d["name"], d["method"].encode(), d["path"].encode()
        self.auth = d.get("auth") == "bearer"
        self.headers = [h.lower() for h in d.get("headers", [])]
        for h in self.headers:
            if not re.fullmatch(r"[a-z][a-z0-9!#$%&'*+.^_`|~-]{0,55}", h):
                raise SystemExit(f"{self.name}: header {h!r}: names start with a letter, are HTTP tokens, at most 56 characters")
            if h in RESERVED_HEADERS: raise SystemExit(f"{self.name}: header {h!r} is handled by the parsers already")
        if len(set(self.headers)) != len(self.headers): raise SystemExit(f"{self.name}: a header listed twice")
        if not re.fullmatch(rb"[A-Z]+", self.method): raise SystemExit(f"{self.name}: odd method")
        if not self.path.startswith(b"/") or b" " in self.path: raise SystemExit(f"{self.name}: odd path")
        ps = re.findall(rb"\{(\w+)\}", self.path)
        if len(ps) > 1: raise SystemExit(f"{self.name}: at most one path parameter is supported")
        self.param = ps[0].decode() if ps else None
        if self.param:
            p = params.get(self.param, {})
            if p.get("type") != "positive-int" or p.get("max_digits", 16) != 16:
                raise SystemExit(f"{self.name}: only positive-int params with max_digits 16 (fp_digits) are supported")
            self.P, self.S = self.path.split(b"{" + self.param.encode() + b"}")
        else:
            self.P, self.S = self.path, None


def load_spec(fn):
    spec = json.load(open(fn))
    if spec.get("http_version", "1.1") != "1.1": raise SystemExit("only HTTP/1.1")
    routes = [Route(d, spec.get("params", {})) for d in spec["routes"]]
    if len({r.name for r in routes}) != len(routes): raise SystemExit("duplicate route names")
    if len(spec_headers(routes)) > 32: raise SystemExit("at most 32 distinct headers (a 32-bit mask per route)")
    return spec, routes


def spec_headers(routes):
    """The spec's headers, lowercase, in order of first appearance: the index is FP_H_<NAME>."""
    out = []
    for r in routes:
        for h in r.headers:
            if h not in out: out.append(h)
    return out


def hdr_enum(h): return "FP_H_" + re.sub(r"[^A-Z0-9]", "_", h.upper())


def methods_in_order(routes):
    out = []
    for r in routes:
        if r.method not in out: out.append(r.method)
    return out


def ref_digits(s):
    if not 1 <= len(s) <= 16 or not all(c in DIGITS for c in s): return False, 0
    v = int(s); return v > 0, v


def route_of(routes, method, path):
    """Exact model of the generated fp_route: (route or None, id_ok, id)."""
    for r in routes:
        if r.method != method: continue
        if r.param is None:
            if path == r.P: return r, False, 0
        elif len(path) >= len(r.P) + len(r.S) and path.startswith(r.P) and path.endswith(r.S):
            mid = path[len(r.P):len(path) - len(r.S)]
            ok, v = ref_digits(mid)
            return (r if ok or b"/" not in mid else None), ok, v
    return None, False, 0


# A canonical family is a list of patterns (one per digit count); a pattern is a list of sets of bytes.
def family(r):
    if r.param is None: return [[{c} for c in r.P]]
    return [[{c} for c in r.P] + [DIGITS] * n + [{c} for c in r.S] for n in range(1, 9)]


def could_match(e, pat):
    """Can earlier route e's matcher accept some path described by pat?"""
    def compat(lit, pos): return all(lit[i] in pat[pos + i] for i in range(len(lit)))
    if e.param is None: return len(e.P) == len(pat) and compat(e.P, 0)
    if len(pat) < len(e.P) + len(e.S): return False
    return compat(e.P, 0) and compat(e.S, len(pat) - len(e.S))


def canonical_ok(routes, x):
    """Is a canonical template for x exactly equivalent to the general path? (else: why not)"""
    tail = (x.S or b"") + VERSION
    if x.param and tail[0] in DIGITS: return "suffix starts with a digit"
    shortest = len(x.method) + 1 + len(x.P) + (1 + len(tail) if x.param else len(VERSION))   # 1 digit
    if shortest < 17: return "request line shorter than 17 bytes (the general path falls back)"
    for e in routes:
        if e is x: break
        if e.method == x.method and any(could_match(e, p) for p in family(x)):
            return f"shadowed by earlier route {e.name}"
    return None


# ---------------- C emission ----------------
def c_str(b):
    return '"' + "".join(chr(c) if 32 <= c < 127 and c not in b'"\\' else f"\\x{c:02x}" for c in b) + '"'


def le(b): return int.from_bytes(b, "little")


def chunk_conds(lit, base):
    """Conditions comparing the bytes lit at base (C expression) with 64-bit loads."""
    out, n = [], len(lit)
    for i in range(0, n - n % 8, 8):
        out.append(f"fp_ld64({base} + {i}) == 0x{le(lit[i:i+8]):016x}ull")
    if n % 8:
        if n >= 8: out.append(f"fp_ld64({base} + {n - 8}) == 0x{le(lit[n-8:]):016x}ull")
        else: out.append(f"(fp_ld64({base}) & 0x{(1 << 8 * n) - 1:x}ull) == 0x{le(lit):x}ull")
    return [c.replace(" + 0)", ")") for c in out]


def gen_fields_region(spec, routes):
    hs = spec_headers(routes)
    L = ["// ==== BEGIN API SPEC FIELDS ====",
         f"// GENERATED by gen/httpgen.py from {spec.get('name', '?')} — do not edit; regenerate.",
         f"// Headers the spec asks for by name: {', '.join(hs) if hs else 'none'}. req_t.hdr[FP_H_<NAME>] (see req_t)."]
    L.append(f"#define FP_NHDR {len(hs)}")
    letters = sorted({h[0] for h in hs} - {"c", "t"})   # c/t lines are candidates already
    L.append("// first letters other than c/t: the backends' line classification marks them too (see fastparse.h)")
    L.append("#define FP_HDR_LETTERS(X)" + "".join(f" X('{c}')" for c in letters))
    if hs:
        L.append("enum { " + ", ".join(hdr_enum(h) for h in hs) + " };")
        L.append("typedef struct { const char *lc; int len; } fp_hdr_name_t;   // lowercase, NUL-padded to 8 bytes (fp_ci_eq)")
        L.append("static const fp_hdr_name_t fp_hdr_names[FP_NHDR] = {" + ", ".join(
            "{" + c_str(h.encode() + b"\0" * (-len(h) % 8)) + f", {len(h)}}}" for h in hs) + "};")
    L.append("// ==== END API SPEC FIELDS ====")
    return "\n".join(L) + "\n"


def gen_spec_region(spec, routes, prof):
    rn = lambda r: "R_" + r.name
    L = []
    w = L.append
    w("// ==== BEGIN API SPEC ====")
    w(f"// GENERATED by gen/httpgen.py from {spec.get('name', '?')} — do not edit; regenerate.")
    w("// Every literal below comes from the spec (methods, paths) or from HTTP itself.")
    if prof: w(f"// Profile-guided decisions from {prof['requests']} profiled requests (numbers only, see bottom).")
    w("enum { R_NOTFOUND, " + ", ".join(rn(r) for r in routes) + ", FP_NROUTES };")
    w(f'#define FP_SPEC_NAME "generated from {spec.get("name", "?")}{" + profile" if prof else ""}"')

    # auth
    auth = [rn(r) for r in routes if r.auth]
    w("FP_INLINE int fp_route_auth(int route) { " + (f"return {' || '.join('route == ' + a for a in auth)};" if auth else "(void)route; return 0;") + " }")

    # the spec's headers: per route mask, and the line matcher (first byte, then longest name first)
    hs = spec_headers(routes); hidx = {h: i for i, h in enumerate(hs)}
    hr = [r for r in routes if r.headers]
    w("// headers the route asks for (bit i = FP_H_ index i)")
    if hr:
        w("FP_INLINE unsigned fp_route_hdrs(int route) {")
        w("  switch (route) {")
        for r in hr: w(f"    case {rn(r)}: return 0x{sum(1 << hidx[h] for h in r.headers):x}u;   // {', '.join(r.headers)}")
        w("    default: return 0;")
        w("  }")
        w("}")
    else:
        w("FP_INLINE unsigned fp_route_hdrs(int route) { (void)route; return 0; }")
    w("// which spec header the line [s, end) is: FP_H_ index, -1 none, -2 it starts with a name but isn't \"name:\"")
    if hs:
        w("FP_INLINE int fp_hdr_match(const char *b, int s, int end) {")
        w("  int bad = 0;")
        w("  switch (b[s] | 0x20) {")
        for c in sorted({h[0] for h in hs}):
            w(f"    case '{c}':")
            for h in sorted((h for h in hs if h[0] == c), key=len, reverse=True):
                n = len(h); lit = c_str(h.encode() + b"\0" * (-n % 8))
                w(f"      if (end - s >= {n} && fp_ci_eq(b + s, {lit}, {n})) {{ if (end - s > {n} && b[s + {n}] == ':') return {hdr_enum(h)}; bad = 1; }}")
            w("      break;")
        w("  }")
        w("  return bad ? -2 : -1;")
        w("}")
    else:
        w("FP_INLINE int fp_hdr_match(const char *b, int s, int end) { (void)b; (void)s; (void)end; return -1; }")

    # walk-core methods: spec (auth) or profile (long header blocks). Routes asking for headers don't need it:
    # their names' first letters are candidate lines in the SIMD classification (FP_HDR_LETTERS)
    walk, why = [], {}
    for m in methods_in_order(routes):
        reason = None
        if any(r.auth for r in routes if r.method == m): reason = "routes need Authorization"
        if prof:
            st = prof["methods"].get(m.decode())
            if st and st["median_header_bytes"] >= 192: reason = (reason + "; " if reason else "") + f"median header block {st['median_header_bytes']} B"
        if reason: walk.append(m); why[m] = reason
    conds = []
    for m in walk:
        key = (m + b" ")[:4]
        conds.append(f"fp_ld32(b) == 0x{le(key):08x}u" if len(key) == 4 else f"(fp_ld32(b) & 0x{(1 << 8 * len(key)) - 1:x}u) == 0x{le(key):x}u")
    w("// walk core for: " + (", ".join(f"{m.decode()} ({why[m]})" for m in walk) if walk else "nothing"))
    w("FP_INLINE int fp_spec_walk(const char *b) { " + (f"return {' || '.join(conds)};" if conds else "(void)b; return 0;") + " }")

    # learned layout per route (never for routes that ask for headers: a layout only compares a line's first
    # 16 bytes, and those values vary)
    if prof:
        on = [r for r in routes if prof["routes"].get(r.name, {}).get("top_layout_share", 0) >= 0.5 and not r.headers]
        w("// learned layout only for routes whose most common layout is >= 50% of their profiled requests")
        w("FP_INLINE int fp_spec_learn(int route) { " + (f"return {' || '.join('route == ' + rn(r) for r in on)};" if on else "(void)route; return 0;") + " }")
    elif hr:
        w("// learned layout everywhere but the routes that ask for headers")
        w("FP_INLINE int fp_spec_learn(int route) { return !(" + " || ".join("route == " + rn(r) for r in hr) + "); }")
    else:
        w("FP_INLINE int fp_spec_learn(int route) { (void)route; return 1; }")
    w("")

    # general router (spec order = first match wins, never reordered)
    w("FP_INLINE void fp_route(const char *m, int ml, const char *p, int pl, req_t *r) {")
    w("  r->route = R_NOTFOUND; r->id_ok = 0; r->id = 0;")
    first_m = True
    for m in methods_in_order(routes):
        w(f"  {'if' if first_m else '} else if'} (ml == {len(m)} && fp_eq(m, {c_str(m)}, {len(m)})) {{"); first_m = False
        first_r = True
        for r in (r for r in routes if r.method == m):
            kw = "if" if first_r else "else if"; first_r = False
            if r.param is None:
                w(f"    {kw} (pl == {len(r.P)} && fp_eq(p, {c_str(r.P)}, {len(r.P)})) r->route = {rn(r)};")
            else:
                cond = f"pl >= {len(r.P) + len(r.S)} && fp_eq(p, {c_str(r.P)}, {len(r.P)})"
                if r.S: cond += f" && fp_eq(p + pl - {len(r.S)}, {c_str(r.S)}, {len(r.S)})"
                n = f"pl - {len(r.P) + len(r.S)}"
                w(f"    {kw} ({cond}) {{")
                w(f"      r->id = fp_digits(p + {len(r.P)}, {n}, &r->id_ok);")
                w(f"      r->route = (r->id_ok || !memchr(p + {len(r.P)}, '/', {n})) ? {rn(r)} : R_NOTFOUND; }}")
    if not first_m: w("  }")
    w("}")
    w("")

    # canonical templates, profile order (they are disjoint by construction, so order is free)
    canon, skipped = [], []
    for r in routes:
        why_not = canonical_ok(routes, r)
        (skipped.append((r, why_not)) if why_not else canon.append(r))
    if prof: canon.sort(key=lambda r: -prof["routes"].get(r.name, {}).get("count", 0))
    w("// Canonical request lines, checked straight from the first bytes (see fp_reqline). Each one was")
    w("// verified by the generator to give exactly what fp_route + fp_reqline give.")
    for r, why_not in skipped: w(f"//   no template for {r.name}: {why_not}")
    for r in routes:   # first match wins: a route no instance of which reaches it is a spec bug
        sample = r.P + (b"1" + r.S if r.param else b"")
        got = route_of(routes, r.method, sample)[0]
        if got is not r:
            first = next(e for e in routes if e.method == r.method and could_match(e, [{c} for c in sample]))
            msg = (f"route {r.name} ({r.method.decode()} {r.path.decode()}) is unreachable: {sample.decode()!r} matches "
                   f"{first.name} first and routes to {got.name if got else 'NOTFOUND'}")
            print("httpgen: warning: " + msg, file=sys.stderr); w("// WARNING: " + msg)
    w("FP_INLINE int fp_canon_ok(req_t *r, int l0) { r->auth = 0; r->tok = 0; r->tok_len = 0; fp_hdr_reset(r); return l0; }")
    w("FP_INLINE int fp_reqline_canon(const char *b, req_t *r) {")
    for r in canon:
        head = r.method + b" " + r.P
        if r.param is None:
            lit = head + VERSION
            w(f"  if ({' && '.join(chunk_conds(lit, 'b'))}) {{   // {r.name}")
            w(f"    r->route = {rn(r)}; r->id_ok = 0; r->id = 0; return fp_canon_ok(r, {len(lit) - 1}); }}")
        else:
            tail = r.S + VERSION
            w(f"  if ({' && '.join(chunk_conds(head, 'b'))}) {{   // {r.name}: {r.path.decode()}, 1..8 digits")
            w(f"    uint64_t d = fp_ld64(b + {len(head)}), nd = fp_nondigit(d); int n = nd ? fp_ctz(nd) >> 3 : 0;")
            w(f"    const char *q = b + {len(head)} + n;")
            w(f"    if (n && {' && '.join(chunk_conds(tail, 'q'))}) {{")
            w(f"      r->route = {rn(r)}; r->id = (int64_t)fp_swar8(d, n); r->id_ok = r->id > 0; return fp_canon_ok(r, {len(head)} + n + {len(tail) - 1}); }}")
            w("  }")
    w("  return 0;")
    w("}")
    if prof:
        w("// profile (numbers only): " + "; ".join(
            f"{k} n={v['count']} layout={v['top_layout_share']:.2f} hdr={v['median_header_bytes']}B" for k, v in prof["routes"].items()))
    w("// ==== END API SPEC ====")
    return "\n".join(L) + "\n"


def cmd_gen(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="httpgen.py gen")
    ap.add_argument("spec"); ap.add_argument("--base", required=True); ap.add_argument("-o", required=True)
    ap.add_argument("--profile")
    a = ap.parse_args(argv)
    spec, routes = load_spec(a.spec)
    prof = json.load(open(a.profile)) if a.profile else None
    if prof:   # numbers only: refuse anything else, so traffic bytes cannot leak into the C
        for v in list(prof["routes"].values()) + list(prof["methods"].values()):
            if not all(isinstance(x, (int, float)) for x in v.values()): raise SystemExit("profile must contain numbers only")
    base = open(a.base).read()
    i, j = base.index("// ==== BEGIN API SPEC FIELDS ===="), base.index("// ==== END API SPEC FIELDS ====")
    j = base.index("\n", j) + 1
    base = base[:i] + gen_fields_region(spec, routes) + base[j:]
    i, j = base.index("// ==== BEGIN API SPEC ===="), base.index("// ==== END API SPEC ====")
    j = base.index("\n", j) + 1
    out = base[:i] + gen_spec_region(spec, routes, prof) + base[j:]
    out = out.replace("// fastparse.h - SIMD fast-path HTTP/1.1 request parser", "// fastparse_gen.h (GENERATED from " + a.spec.split("/")[-1] + (" + " + a.profile.split("/")[-1] if a.profile else "") + " by gen/httpgen.py) - SIMD fast-path HTTP/1.1 request parser", 1)
    open(a.o, "w").write(out)


# ---------------- profile ----------------
def cmd_profile(argv):
    spec, routes = load_spec(argv[0])
    data = open(argv[1], "rb").read()
    per_route, per_method, pos, n = defaultdict(list), defaultdict(list), 0, 0
    while True:
        e = data.find(b"\r\n\r\n", pos)
        if e < 0: break
        head = data[pos:e + 4]
        lines = head.split(b"\r\n")
        parts = lines[0].split(b" ")
        cl = 0
        for h in lines[1:]:
            k, _, v = h.partition(b":")
            if k.strip().lower() == b"content-length" and v.strip().isdigit(): cl = int(v.strip())
        if len(parts) == 3:
            r, _, _ = route_of(routes, parts[0], parts[1])
            layout = tuple((h.partition(b":")[0].lower(), len(h)) for h in lines[1:] if h)
            hb = len(head) - len(lines[0]) - 2
            per_route[r.name if r else "NOTFOUND"].append((layout, hb))
            per_method[parts[0].decode()].append(hb)
            n += 1
        pos = e + 4 + cl
    prof = {"requests": n, "routes": {}, "methods": {}}
    for name, xs in per_route.items():
        top = Counter(l for l, _ in xs).most_common(1)[0][1]
        prof["routes"][name] = {"count": len(xs), "top_layout_share": round(top / len(xs), 4),
                                "median_header_bytes": int(statistics.median(h for _, h in xs))}
    for m, hs in per_method.items():
        prof["methods"][m] = {"count": len(hs), "median_header_bytes": int(statistics.median(hs))}
    json.dump(prof, sys.stdout, indent=1); print()


# ---------------- self-check vectors ----------------
def cmd_vectors(argv):
    """vectors SPEC N SEED: random (method, path) + the router model's expected result, one per line,
    hex-encoded: METHOD PATH ROUTE_INDEX ID_OK ID (route index 0 = NOTFOUND). Checked by check_gen.c."""
    import random
    spec, routes = load_spec(argv[0]); rnd = random.Random(int(argv[2]))
    idx = {r.name: i + 1 for i, r in enumerate(routes)}
    pieces = [b"0", b"1", b"7", b"00", b"12", b"0000000000000001", b"12345678", b"123456789", b"99999999999999999",
              b"x", b"-1", b"1.5", b"/", b"", b"2", b"new", b"7/", b" "]
    meths = methods_in_order(routes) + [b"GET", b"POST", b"PATCH"]
    for _ in range(int(argv[1])):
        r = rnd.choice(routes); m = r.method if rnd.random() < 0.9 else rnd.choice(meths)
        if r.param: path = r.P + b"".join(rnd.choice(pieces) for _ in range(rnd.choice([1, 1, 1, 2]))) + r.S
        else: path = r.P
        if rnd.random() < 0.2 and path:   # damage
            i = rnd.randrange(len(path)); path = path[:i] + rnd.choice([b"", b"/", b"1", b"a", b"x"]) + path[i + 1:]
        if not path.startswith(b"/") or b" " in path: continue
        got, ok, v = route_of(routes, m, path)
        print(m.hex(), path.hex(), idx[got.name] if got else 0, int(ok), v if ok else 0)


if __name__ == "__main__":
    cmds = {"gen": cmd_gen, "profile": cmd_profile, "vectors": cmd_vectors}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds: raise SystemExit(__doc__)
    cmds[sys.argv[1]](sys.argv[2:])
