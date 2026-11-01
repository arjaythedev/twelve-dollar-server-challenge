// Feed API: one Selector loop per process, SQLite in-process (via FFM), group commit per loop iteration.
// See README.md for the design. Config comes only from SQLITE_PATH, JWT_SECRET, HOST, PORT.
//
// A note from the author, who was told "port it to Java, it'll be fun":
// It was not fun. This file is a C program wearing a Java costume it bought at a gas station. There are
// no beans. There is no XML. There is no AbstractSingletonProxyFactoryBean, no dependency injection, no
// annotation that secretly opens a database connection when you look at it funny. If you came here
// looking for a FeedPostLikeRequestHandlerStrategyFactoryImpl, go back to your enterprise and tell them
// a man with a hex editor sent you.
//
// Rules I followed so the JIT and the garbage collector leave me the hell alone:
//   - one thread, one Selector (epoll underneath, because Linux did the real work, as usual)
//   - no allocation per request on the hot path: static byte[] buffers, static parser state, longs as
//     pointers. Every "new" on the request path is a little GC pause I have to apologize for later.
//   - SQLite is the real C library, called through java.lang.foreign, because JDBC is a crime scene.
import java.io.IOException;
import java.io.InputStream;
import java.lang.foreign.*;
import java.lang.invoke.MethodHandle;
import java.net.InetSocketAddress;
import java.net.StandardProtocolFamily;
import java.net.StandardSocketOptions;
import java.nio.ByteBuffer;
import java.nio.channels.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Consumer;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import static java.lang.foreign.ValueLayout.*;

public final class Server {
  // Easter egg #1: the number of AbstractSingletonProxyFactoryBeans instantiated by this program.
  // If this is ever not zero, burn the box down and collect the insurance.
  static final int ABSTRACT_SINGLETON_PROXY_FACTORY_BEAN_COUNT = 0;
  // Easter egg #2: compile-time false, so javac deletes the branch below. Like Oracle deletes your weekend.
  static final boolean ORACLE_LAWYERS_ARE_WATCHING = false;

  static final int RBUF = 64 * 1024;
  static final int MAXREQ = 64 * 1024;     // header + body cap per request
  static final int BATCH_OUT = 4 << 20;    // response bytes held per loop iteration until its commit

  // ---------- SQLite through FFM ----------
  // Pointers are passed as plain longs (same register, same ABI on x86-64), so there are no wrapper
  // objects per call. Small calls are "critical": no thread-state dance, because a column read does
  // not need a ceremony with a priest and two notaries.
  static final Linker LINKER = Linker.nativeLinker();
  static final SymbolLookup LIB = SymbolLookup.libraryLookup(Path.of(System.getProperty("java.class.path"), "libsqlite3.so"), Arena.global());
  static final MemorySegment MEM = MemorySegment.NULL.reinterpret(Long.MAX_VALUE);  // all of memory. Yes, all of it. Java can't stop me now.
  static MethodHandle fn(String name, boolean critical, MemoryLayout ret, MemoryLayout... args) {
    MemorySegment f = LIB.find(name).orElseThrow();
    FunctionDescriptor d = FunctionDescriptor.of(ret, args);
    return critical ? LINKER.downcallHandle(f, d, Linker.Option.critical(false)) : LINKER.downcallHandle(f, d);
  }
  static final MethodHandle
    INIT = fn("sqlite3_initialize", false, JAVA_INT),
    OPEN = fn("sqlite3_open_v2", false, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_INT, JAVA_LONG),
    BUSY = fn("sqlite3_busy_timeout", false, JAVA_INT, JAVA_LONG, JAVA_INT),
    EXEC = fn("sqlite3_exec", false, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG),
    PREP = fn("sqlite3_prepare_v3", false, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG),
    STEP = fn("sqlite3_step", false, JAVA_INT, JAVA_LONG),
    RESET = fn("sqlite3_reset", true, JAVA_INT, JAVA_LONG),
    BIND_I64 = fn("sqlite3_bind_int64", true, JAVA_INT, JAVA_LONG, JAVA_INT, JAVA_LONG),
    BIND_TEXT = fn("sqlite3_bind_text", true, JAVA_INT, JAVA_LONG, JAVA_INT, JAVA_LONG, JAVA_INT, JAVA_LONG),
    COL_I64 = fn("sqlite3_column_int64", true, JAVA_LONG, JAVA_LONG, JAVA_INT),
    COL_TEXT = fn("sqlite3_column_text", true, JAVA_LONG, JAVA_LONG, JAVA_INT),
    COL_BYTES = fn("sqlite3_column_bytes", true, JAVA_INT, JAVA_LONG, JAVA_INT),
    CHANGES = fn("sqlite3_changes", true, JAVA_INT, JAVA_LONG),
    AUTOCOMMIT = fn("sqlite3_get_autocommit", true, JAVA_INT, JAVA_LONG),
    ERRMSG = fn("sqlite3_errmsg", false, JAVA_LONG, JAVA_LONG),
    CKPT = fn("sqlite3_wal_checkpoint_v2", false, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_INT, JAVA_LONG, JAVA_LONG);

  // invokeExact "throws Throwable". Not Exception. THROWABLE. The method signature equivalent of a
  // shrug. So every call gets wrapped, because checked exceptions are a tax on the honest.
  static RuntimeException fuck(Throwable t) { return t instanceof RuntimeException r ? r : new RuntimeException(t); }
  static int init() { try { return (int) INIT.invokeExact(); } catch (Throwable t) { throw fuck(t); } }
  static int open(long path, long pdb, int flags) { try { return (int) OPEN.invokeExact(path, pdb, flags, 0L); } catch (Throwable t) { throw fuck(t); } }
  static int busy(long db, int ms) { try { return (int) BUSY.invokeExact(db, ms); } catch (Throwable t) { throw fuck(t); } }
  static int exec(long db, String sql) { try { return (int) EXEC.invokeExact(db, cstr(sql), 0L, 0L, 0L); } catch (Throwable t) { throw fuck(t); } }
  static int step(long s) { try { return (int) STEP.invokeExact(s); } catch (Throwable t) { throw fuck(t); } }
  static int reset(long s) { try { return (int) RESET.invokeExact(s); } catch (Throwable t) { throw fuck(t); } }
  static void bind(long s, int i, long v) { try { int r = (int) BIND_I64.invokeExact(s, i, v); } catch (Throwable t) { throw fuck(t); } }
  static void bindText(long s, int i, long p, int n) { try { int r = (int) BIND_TEXT.invokeExact(s, i, p, n, 0L); } catch (Throwable t) { throw fuck(t); } }
  static long colI64(long s, int i) { try { return (long) COL_I64.invokeExact(s, i); } catch (Throwable t) { throw fuck(t); } }
  static long colText(long s, int i) { try { return (long) COL_TEXT.invokeExact(s, i); } catch (Throwable t) { throw fuck(t); } }
  static int colBytes(long s, int i) { try { return (int) COL_BYTES.invokeExact(s, i); } catch (Throwable t) { throw fuck(t); } }
  static int changes(long db) { try { return (int) CHANGES.invokeExact(db); } catch (Throwable t) { throw fuck(t); } }
  static boolean autocommit(long db) { try { return (int) AUTOCOMMIT.invokeExact(db) != 0; } catch (Throwable t) { throw fuck(t); } }
  static String errmsg(long db) { try { return MemorySegment.ofAddress((long) ERRMSG.invokeExact(db)).reinterpret(4096).getString(0); } catch (Throwable t) { throw fuck(t); } }
  static int ckpt(long db, long plog, long pdone) { try { return (int) CKPT.invokeExact(db, 0L, 0, plog, pdone); } catch (Throwable t) { throw fuck(t); } }
  static long cstr(String s) { return Arena.global().allocateFrom(s).address(); }  // startup only, leaks on purpose, like a JavaBean

  static final int ROW = 100, DONE = 101;

  // ---------- output: one arena per loop iteration, sent after COMMIT ----------
  static final byte[] A = new byte[BATCH_OUT];
  static final ByteBuffer OUT = ByteBuffer.allocateDirect(BATCH_OUT);  // A is copied here once per batch, then written
  static int alen;
  static Conn[] segC = new Conn[1024]; static int[] segOff = new int[1024], segLen = new int[1024]; static int nsegs;
  static Conn[] deferred = new Conn[1024]; static int ndef;
  static boolean arenaFull, inTxn, txnWrites, batchFailed, closingFlag;

  static byte[] b(String s) { return s.getBytes(StandardCharsets.UTF_8); }
  static void put(byte[] s) { System.arraycopy(s, 0, A, alen, s.length); alen += s.length; }
  static void put(byte[] s, int off, int n) { System.arraycopy(s, off, A, alen, n); alen += n; }
  static void putu(long v) {
    int n = 1; for (long t = v; t >= 10; t /= 10) n++;
    alen += n;
    for (int i = alen - 1; n-- > 0; i--) { A[i] = (byte) ('0' + v % 10); v /= 10; }
  }
  static final byte[] HEX = b("0123456789abcdef");
  static void putjs(byte[] s, int off, int n) {  // JSON string contents, escaped
    int run = off, end = off + n;
    for (int i = off; i < end; i++) {
      int c = s[i] & 0xff;
      if (c >= 0x20 && c != '"' && c != '\\') continue;
      put(s, run, i - run); run = i + 1;
      A[alen++] = '\\';
      switch (c) {
        case '"' -> A[alen++] = '"';
        case '\\' -> A[alen++] = '\\';
        case '\n' -> A[alen++] = 'n';
        case '\r' -> A[alen++] = 'r';
        case '\t' -> A[alen++] = 't';
        default -> { A[alen++] = 'u'; A[alen++] = '0'; A[alen++] = '0'; A[alen++] = HEX[c >> 4]; A[alen++] = HEX[c & 15]; }
      }
    }
    put(s, run, end - run);
  }
  static byte[] TMP = new byte[8192];
  static void putCol(long st, int i, boolean escape) {  // copies a text column out of SQLite's memory into the arena
    long p = colText(st, i); int n = colBytes(st, i);
    if (!escape) { MemorySegment.copy(MEM, JAVA_BYTE, p, A, alen, n); alen += n; return; }
    if (n > TMP.length) TMP = new byte[n * 2];
    MemorySegment.copy(MEM, JAVA_BYTE, p, TMP, 0, n);
    putjs(TMP, 0, n);
  }

  // response head with a blank Content-Length that finish() fills in
  static final byte[] S200 = b("HTTP/1.1 200 OK\r\n"), S201 = b("HTTP/1.1 201 Created\r\n"), S400 = b("HTTP/1.1 400 Bad Request\r\n"),
    S401 = b("HTTP/1.1 401 Unauthorized\r\n"), S404 = b("HTTP/1.1 404 Not Found\r\n"), S503 = b("HTTP/1.1 503 Service Unavailable\r\n"),
    S500 = b("HTTP/1.1 500 Internal Server Error\r\n"), CLOSE_HDR = b("Connection: close\r\n"),
    HEAD = b("Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
  static int bodyStart, clPos;
  static void beginResp(int status, boolean close) {
    put(switch (status) { case 200 -> S200; case 201 -> S201; case 400 -> S400; case 401 -> S401; case 404 -> S404; case 503 -> S503; default -> S500; });
    if (close) put(CLOSE_HDR);
    put(HEAD);
    clPos = alen - 10; bodyStart = alen;
  }
  static void finish() {
    int n = alen - bodyStart, p = clPos + 6;
    do { A[--p] = (byte) ('0' + n % 10); n /= 10; } while (n > 0);
  }
  static final byte[] E_OPEN = b("{\"error\":\""), E_CLOSE = b("\"}");
  static void err(int status, byte[] msg, boolean close) { beginResp(status, close); put(E_OPEN); put(msg); put(E_CLOSE); finish(); }
  static final byte[] M_INTERNAL = b("internal server error"), M_BADID = b("invalid post id"), M_NOPOST = b("post not found"),
    M_NOTFOUND = b("not found"), M_MALFORMED = b("malformed JSON body"), M_REQUIRED = b("body is required"),
    M_TOOLONG = b("body must be at most 500 characters"), M_MISSING = b("missing bearer token"),
    M_BAD = b("invalid or expired token"), M_PAYLOAD = b("invalid token payload");

  // ---------- base64url ----------
  static final byte[] B64 = new byte[256];
  static int b64dec(byte[] s, int off, int n, byte[] out) {  // -1 on bad input
    if (n % 4 == 1) return -1;
    int acc = 0, bits = 0, o = 0;
    for (int i = off; i < off + n; i++) {
      int v = B64[s[i] & 0xff]; if (v < 0) return -1;
      acc = acc << 6 | v; bits += 6;
      if (bits >= 8) { bits -= 8; out[o++] = (byte) (acc >> bits); }
    }
    return o;
  }

  // ---------- JSON (strict, RFC 8259) ----------
  // Parser state lives in statics. Thread-safe? There is one thread. Spring Boot fans, look away.
  static byte[] jb; static int jp, je;
  static void ws() { while (jp < je) { byte c = jb[jp]; if (c == ' ' || c == '\t' || c == '\n' || c == '\r') jp++; else break; } }
  static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
  static int hex4(byte[] s, int p) { int a = hexv(s[p]), b = hexv(s[p + 1]), c = hexv(s[p + 2]), d = hexv(s[p + 3]); return (a | b | c | d) < 0 ? -1 : a << 12 | b << 8 | c << 4 | d; }
  static int utf8Len(byte[] s, int p, int e) {  // length of one valid UTF-8 sequence, 0 if invalid
    int c = s[p] & 0xff;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) return e - p >= 2 && (s[p + 1] & 0xc0) == 0x80 ? 2 : 0;
    if (c >= 0xe0 && c <= 0xef) {
      if (e - p < 3 || (s[p + 1] & 0xc0) != 0x80 || (s[p + 2] & 0xc0) != 0x80) return 0;
      int c1 = s[p + 1] & 0xff;
      if ((c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 > 0x9f)) return 0;
      return 3;
    }
    if (c >= 0xf0 && c <= 0xf4) {
      if (e - p < 4 || (s[p + 1] & 0xc0) != 0x80 || (s[p + 2] & 0xc0) != 0x80 || (s[p + 3] & 0xc0) != 0x80) return 0;
      int c1 = s[p + 1] & 0xff;
      if ((c == 0xf0 && c1 < 0x90) || (c == 0xf4 && c1 > 0x8f)) return 0;
      return 4;
    }
    return 0;
  }
  // Parses the string at jp (on its opening quote), decoding UTF-8 into out when out is non-null.
  // Returns the decoded length, -1 on a syntax error, -2 if it doesn't fit in cap (the string is still validated).
  static int jstr(byte[] out, int cap) {
    byte[] s = jb; int p = jp + 1, e = je, o = 0; boolean over = false;
    while (p < e) {
      int c = s[p] & 0xff;
      if (c == '"') { jp = p + 1; return over ? -2 : o; }
      if (c < 0x20) return -1;
      if (c == '\\') {
        if (++p >= e) return -1;
        int ch;
        switch (s[p]) {
          case '"': ch = '"'; break; case '\\': ch = '\\'; break; case '/': ch = '/'; break;
          case 'b': ch = '\b'; break; case 'f': ch = '\f'; break; case 'n': ch = '\n'; break;
          case 'r': ch = '\r'; break; case 't': ch = '\t'; break;
          case 'u': {
            if (e - p < 5) return -1;
            int cp = hex4(s, p + 1); if (cp < 0) return -1;
            p += 4;
            if (cp >= 0xd800 && cp <= 0xdbff && e - p >= 7 && s[p + 1] == '\\' && s[p + 2] == 'u') {
              int lo = hex4(s, p + 3); if (lo < 0) return -1;
              if (lo >= 0xdc00 && lo <= 0xdfff) { cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
            }
            if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;  // lone surrogate: U+FFFD, as JS TextEncoder does
            int ul = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            if (out != null && o + ul > cap) { out = null; over = true; }
            if (out != null) switch (ul) {
              case 1 -> out[o] = (byte) cp;
              case 2 -> { out[o] = (byte) (0xc0 | cp >> 6); out[o + 1] = (byte) (0x80 | (cp & 63)); }
              case 3 -> { out[o] = (byte) (0xe0 | cp >> 12); out[o + 1] = (byte) (0x80 | (cp >> 6 & 63)); out[o + 2] = (byte) (0x80 | (cp & 63)); }
              default -> { out[o] = (byte) (0xf0 | cp >> 18); out[o + 1] = (byte) (0x80 | (cp >> 12 & 63)); out[o + 2] = (byte) (0x80 | (cp >> 6 & 63)); out[o + 3] = (byte) (0x80 | (cp & 63)); }
            }
            o += ul; p++; continue;
          }
          default: return -1;
        }
        if (out != null && o >= cap) { out = null; over = true; }
        if (out != null) out[o] = (byte) ch;
        o++; p++; continue;
      }
      int l = utf8Len(s, p, e); if (l == 0) return -1;
      if (out != null && o + l > cap) { out = null; over = true; }
      if (out != null) System.arraycopy(s, p, out, o, l);
      o += l; p += l;
    }
    return -1;
  }
  static int jstrAt(byte[] src, int v, int ve, byte[] out, int cap) { jb = src; jp = v; je = ve; return jstr(out, cap); }
  static boolean digit(byte c) { return c >= '0' && c <= '9'; }
  static boolean jnumber() {
    int p = jp, e = je; byte[] s = jb;
    if (p < e && s[p] == '-') p++;
    if (p >= e) return false;
    if (s[p] == '0') p++; else if (s[p] >= '1' && s[p] <= '9') while (p < e && digit(s[p])) p++; else return false;
    if (p < e && s[p] == '.') { p++; if (p >= e || !digit(s[p])) return false; while (p < e && digit(s[p])) p++; }
    if (p < e && (s[p] == 'e' || s[p] == 'E')) { p++; if (p < e && (s[p] == '+' || s[p] == '-')) p++; if (p >= e || !digit(s[p])) return false; while (p < e && digit(s[p])) p++; }
    jp = p; return true;
  }
  static final byte[] TRUE = b("true"), FALSE = b("false"), NULL = b("null");
  static boolean jlit(byte[] lit) {
    if (je - jp < lit.length || !Arrays.equals(jb, jp, jp + lit.length, lit, 0, lit.length)) return false;
    jp += lit.length; return true;
  }
  static boolean jvalue(int depth) {  // validates and skips one value
    if (depth > 64) return false;
    ws(); if (jp >= je) return false;
    switch (jb[jp]) {
      case '"': return jstr(null, 0) >= 0;
      case '{':
        jp++; ws();
        if (jp < je && jb[jp] == '}') { jp++; return true; }
        for (;;) {
          ws(); if (jp >= je || jb[jp] != '"' || jstr(null, 0) < 0) return false;
          ws(); if (jp >= je || jb[jp++] != ':') return false;
          if (!jvalue(depth + 1)) return false;
          ws(); if (jp >= je) return false;
          if (jb[jp] == ',') { jp++; continue; }
          if (jb[jp] == '}') { jp++; return true; }
          return false;
        }
      case '[':
        jp++; ws();
        if (jp < je && jb[jp] == ']') { jp++; return true; }
        for (;;) {
          if (!jvalue(depth + 1)) return false;
          ws(); if (jp >= je) return false;
          if (jb[jp] == ',') { jp++; continue; }
          if (jb[jp] == ']') { jp++; return true; }
          return false;
        }
      case 't': return jlit(TRUE);
      case 'f': return jlit(FALSE);
      case 'n': return jlit(NULL);
      default: return jnumber();
    }
  }
  // Validates a whole document. For a top-level object it records the raw span [wv, wve) of the last value
  // of each wanted key (the last duplicate wins, as in JSON.parse), wv = -1 if absent. False if invalid.
  static final int[] wv = new int[4], wve = new int[4];
  static final byte[] KB = new byte[32];
  static boolean jdoc(byte[] s, int off, int n, byte[][] keys) {
    jb = s; jp = off; je = off + n;
    for (int i = 0; i < keys.length; i++) wv[i] = -1;
    ws();
    if (jp < je && jb[jp] == '{') {
      jp++; ws();
      if (jp < je && jb[jp] == '}') jp++;
      else for (;;) {
        ws();
        if (jp >= je || jb[jp] != '"') return false;
        int kl = jstr(KB, KB.length);
        if (kl == -1) return false;
        ws(); if (jp >= je || jb[jp++] != ':') return false;
        ws(); int vs = jp;
        if (!jvalue(1)) return false;
        if (kl >= 0) for (int i = 0; i < keys.length; i++) if (Arrays.equals(KB, 0, kl, keys[i], 0, keys[i].length)) { wv[i] = vs; wve[i] = jp; }
        ws(); if (jp >= je) return false;
        if (jb[jp] == ',') { jp++; continue; }
        if (jb[jp] == '}') { jp++; break; }
        return false;
      }
    } else if (!jvalue(0)) return false;
    ws();
    return jp == je;
  }

  // ---------- auth ----------
  static Mac MAC;  // HmacSHA256 from the JDK: the one part of Java that is faster than what I'd write by hand
  static final byte[] SIG = new byte[48], MACOUT = new byte[32], HDR = new byte[512], PL = new byte[2048], ALG = new byte[8], SB = new byte[24], UNAME = new byte[1024];
  static final byte[][] K_ALG = {b("alg")}, K_PAYLOAD = {b("exp"), b("nbf"), b("sub"), b("username")}, K_BODY = {b("body")};
  static final byte[] BEARER = b("Bearer "), HS256 = b("HS256");
  static long authUid; static int unameLen;
  static int indexOf(byte[] s, int c, int from, int to) { for (int i = from; i < to; i++) if (s[i] == c) return i; return -1; }
  static boolean eq(byte[] s, int off, int n, byte[] lit) { return n == lit.length && Arrays.equals(s, off, off + n, lit, 0, n); }
  static double num(byte[] s, int v, int ve) {  // a JSON number already validated by jdoc
    boolean neg = s[v] == '-'; int i = neg ? v + 1 : v; long x = 0;
    if (ve - i <= 18) {
      for (; i < ve && digit(s[i]); i++) x = x * 10 + s[i] - '0';
      if (i == ve) return neg ? -x : x;
    }
    return Double.parseDouble(new String(s, v, ve - v, StandardCharsets.ISO_8859_1));  // fractions and exponents: rare, allowed to allocate
  }
  // Returns null and fills authUid/UNAME on success, else the message to send with the 401.
  static byte[] auth(byte[] s, int h, int hl) {
    if (h < 0 || hl < 7 || !Arrays.equals(s, h, h + 7, BEARER, 0, 7)) return M_MISSING;
    int t = h + 7, te = h + hl, d1 = indexOf(s, '.', t, te);
    if (d1 < 0) return M_BAD;
    int d2 = indexOf(s, '.', d1 + 1, te);
    if (d2 < 0 || indexOf(s, '.', d2 + 1, te) >= 0) return M_BAD;
    if (te - d2 - 1 != 43 || b64dec(s, d2 + 1, 43, SIG) != 32) return M_BAD;
    MAC.update(s, t, d2 - t);
    try { MAC.doFinal(MACOUT, 0); } catch (javax.crypto.ShortBufferException ex) { throw fuck(ex); }  // a 32-byte buffer for a 32-byte MAC. "But what if it's short?" It isn't, Java. It isn't.
    int diff = 0; for (int i = 0; i < 32; i++) diff |= SIG[i] ^ MACOUT[i];
    if (diff != 0) return M_BAD;
    if (d1 - t > 680 || d2 - d1 - 1 > 2700) return M_BAD;
    int hn = b64dec(s, t, d1 - t, HDR), pn = b64dec(s, d1 + 1, d2 - d1 - 1, PL);
    if (hn < 0 || pn < 0) return M_BAD;
    if (!jdoc(HDR, 0, hn, K_ALG) || wv[0] < 0 || HDR[wv[0]] != '"') return M_BAD;
    int al = jstrAt(HDR, wv[0], wve[0], ALG, ALG.length);
    if (al != 5 || !Arrays.equals(ALG, 0, 5, HS256, 0, 5)) return M_BAD;
    if (!jdoc(PL, 0, pn, K_PAYLOAD) || wv[0] < 0) return M_BAD;  // exp is required
    long now = System.currentTimeMillis() / 1000;
    for (int i = 0; i < 2; i++) if (wv[i] >= 0) {  // exp / nbf must be numbers, checked against now
      if (PL[wv[i]] != '-' && !digit(PL[wv[i]])) return M_BAD;
      if (wve[i] - wv[i] >= 64) return M_BAD;
      double v = num(PL, wv[i], wve[i]);
      if (i == 0 ? now >= v : now < v) return M_BAD;
    }
    int sv = wv[2], sve = wve[2], uv = wv[3], uve = wve[3];
    if (sv < 0 || PL[sv] != '"' || uv < 0 || PL[uv] != '"') return M_PAYLOAD;
    int sl = jstrAt(PL, sv, sve, SB, SB.length);
    if (sl <= 0 || sl > 15) return M_PAYLOAD;
    long v = 0; for (int i = 0; i < sl; i++) { if (!digit(SB[i])) return M_PAYLOAD; v = v * 10 + SB[i] - '0'; }
    if (v <= 0) return M_PAYLOAD;
    int ul = jstrAt(PL, uv, uve, UNAME, UNAME.length);
    if (ul < 0) return M_PAYLOAD;
    authUid = v; unameLen = ul; return null;
  }

  // ---------- database ----------
  static long db, stFeed, stPost, stIns, stLike, stExists, stOne, stBegin, stBeginImm, stCommit, stRollback;
  static boolean txn() {  // opens the batch transaction on first use
    if (inTxn) return true;
    int rc = step(stBegin); reset(stBegin);
    return inTxn = rc == DONE;
  }
  // Runs a write inside the batch transaction. With several processes, a read snapshot can't be upgraded
  // once another process has committed (SQLITE_BUSY_SNAPSHOT): end the read-only part of the batch, take
  // the write lock with BEGIN IMMEDIATE (which waits up to busy_timeout) and retry once.
  static int wstep(long s) {
    int rc = step(s);
    if ((rc & 0xff) == 5 && !txnWrites) {
      reset(s);
      if (inTxn) { step(stCommit); reset(stCommit); inTxn = false; }
      rc = step(stBeginImm); reset(stBeginImm);
      if (rc != DONE) return rc;
      inTxn = true;
      rc = step(s);
    }
    if (rc == ROW || rc == DONE) txnWrites = true;
    return rc;
  }
  static void dbFail() {  // a failed statement may have rolled the whole batch back
    if (inTxn && autocommit(db)) { inTxn = false; if (txnWrites) batchFailed = true; txnWrites = false; }
  }
  static final byte[] P_ID = b("{\"id\":"), P_BODY = b(",\"body\":\""), P_CREATED = b("\",\"created_at\":\""), P_AUTHOR = b("\",\"author\":\""),
    P_LIKES = b("\",\"like_count\":"), RBRACE = b("}");
  static void putPostRow(long s) {
    put(P_ID); putu(colI64(s, 0));
    put(P_BODY); putCol(s, 1, true);
    put(P_CREATED); putCol(s, 2, false);
    put(P_AUTHOR); putCol(s, 3, true);
    put(P_LIKES); putu(colI64(s, 4)); put(RBRACE);
  }

  // ---------- request handling ----------
  static long parseId(byte[] s, int off, int n) {  // positive decimal integer, else -1 (huge ids map to MAX: not found)
    if (n == 0) return -1;
    long v = 0;
    for (int i = off; i < off + n; i++) { if (!digit(s[i])) return -1; if (v < 100000000000000000L) v = v * 10 + s[i] - '0'; else v = Long.MAX_VALUE; }
    return v > 0 ? v : -1;
  }
  static int jswsAt(byte[] s, int p, int e) {  // byte length of JS String.trim whitespace at p, or 0
    int c0 = s[p] & 0xff;
    if (c0 == ' ' || (c0 >= 9 && c0 <= 13)) return 1;
    if (e - p >= 2 && c0 == 0xc2 && (s[p + 1] & 0xff) == 0xa0) return 2;
    if (e - p >= 3) {
      int c = c0 << 16 | (s[p + 1] & 0xff) << 8 | (s[p + 2] & 0xff);
      if (c == 0xe19a80 || (c >= 0xe28080 && c <= 0xe2808a) || c == 0xe280a8 || c == 0xe280a9 || c == 0xe280af || c == 0xe2819f || c == 0xe38080 || c == 0xefbbbf) return 3;
    }
    return 0;
  }
  static int jswsBefore(byte[] s, int st, int e) {
    int c = s[e - 1] & 0xff;
    if (c == ' ' || (c >= 9 && c <= 13)) return 1;
    if (e - st >= 2 && (s[e - 2] & 0xff) == 0xc2 && c == 0xa0) return 2;
    if (e - st >= 3 && jswsAt(s, e - 3, e) == 3) return 3;
    return 0;
  }

  static final byte[] GET = b("GET"), POST = b("POST"), FEED = b("/feed"), POSTS_ = b("/posts/"), POSTS = b("/posts"), LIKE = b("/like"), HEALTH = b("/health"),
    FEED_OPEN = b("{\"posts\":["), FEED_CLOSE = b("]}"), POST_OPEN = b("{\"post\":"), LIKED = b("{\"liked\":true,\"already_liked\":"),
    LIKE_ID = b(",\"post_id\":"), NEW_ID = b("{\"post\":{\"id\":"), NEW_TAIL = b("\",\"like_count\":0}}"),
    HEALTH_OK = b("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"), HEALTH_BAD = b("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
  static final byte[] TXT = new byte[MAXREQ];
  static final MemorySegment TXT_NATIVE = Arena.global().allocate(MAXREQ);  // SQLite wants the body in native memory
  static long started;

  static void handle(byte[] s, int m, int ml, int path, int pl, int ah, int al, int body, int bl, boolean close) {
    int q = indexOf(s, '?', path, path + pl); if (q >= 0) pl = q - path;
    boolean get = eq(s, m, ml, GET), post = eq(s, m, ml, POST);
    int rc;
    if (get && eq(s, path, pl, FEED)) {
      if (!txn()) { err(500, M_INTERNAL, close); return; }
      int mark = alen;
      beginResp(200, close); put(FEED_OPEN);
      int i = 0;
      while ((rc = step(stFeed)) == ROW) { if (i++ > 0) A[alen++] = ','; putPostRow(stFeed); }
      reset(stFeed);
      if (rc != DONE) { dbFail(); alen = mark; err(500, M_INTERNAL, close); return; }
      put(FEED_CLOSE); finish(); return;
    }
    if (pl > 7 && Arrays.equals(s, path, path + 7, POSTS_, 0, 7)) {
      int seg = path + 7, sl = indexOf(s, '/', seg, path + pl);
      if (get && sl < 0) {
        long id = parseId(s, seg, pl - 7);
        if (id < 0) { err(400, M_BADID, close); return; }
        if (!txn()) { err(500, M_INTERNAL, close); return; }
        bind(stPost, 1, id);
        int mark = alen;
        rc = step(stPost);
        if (rc == ROW) { beginResp(200, close); put(POST_OPEN); putPostRow(stPost); put(RBRACE); finish(); }
        else if (rc == DONE) err(404, M_NOPOST, close);
        else { dbFail(); alen = mark; err(500, M_INTERNAL, close); }
        reset(stPost); return;
      }
      if (post && sl >= 0 && eq(s, sl, path + pl - sl, LIKE)) {
        byte[] e = auth(s, ah, al);
        if (e != null) { err(401, e, close); return; }
        long id = parseId(s, seg, sl - seg);
        if (id < 0) { err(400, M_BADID, close); return; }
        if (!txn()) { err(500, M_INTERNAL, close); return; }
        bind(stLike, 1, authUid); bind(stLike, 2, id);
        rc = wstep(stLike); reset(stLike);
        if (rc != DONE) { dbFail(); err(500, M_INTERNAL, close); return; }
        boolean created = changes(db) > 0;
        if (!created) {
          bind(stExists, 1, id);
          rc = step(stExists); reset(stExists);
          if (rc == DONE) { err(404, M_NOPOST, close); return; }
          if (rc != ROW) { dbFail(); err(500, M_INTERNAL, close); return; }
        }
        beginResp(created ? 201 : 200, close);
        put(LIKED); put(created ? FALSE : TRUE);
        put(LIKE_ID); putu(id); put(RBRACE); finish(); return;
      }
    }
    if (post && eq(s, path, pl, POSTS)) {
      byte[] e = auth(s, ah, al);
      if (e != null) { err(401, e, close); return; }
      if (!jdoc(s, body, bl, K_BODY)) { err(400, M_MALFORMED, close); return; }
      if (wv[0] < 0 || s[wv[0]] != '"') { err(400, M_REQUIRED, close); return; }
      int n = jstrAt(s, wv[0], wve[0], TXT, TXT.length);
      if (n < 0) { err(400, M_MALFORMED, close); return; }
      int st = 0, se = n, l;
      while (st < se && (l = jswsAt(TXT, st, se)) > 0) st += l;
      while (se > st && (l = jswsBefore(TXT, st, se)) > 0) se -= l;
      if (st == se) { err(400, M_REQUIRED, close); return; }
      int chars = 0; for (int c = st; c < se; c++) if ((TXT[c] & 0xc0) != 0x80) chars++;
      if (chars > 500) { err(400, M_TOOLONG, close); return; }
      if (!txn()) { err(500, M_INTERNAL, close); return; }
      MemorySegment.copy(TXT, st, TXT_NATIVE, JAVA_BYTE, 0, se - st);
      bind(stIns, 1, authUid); bindText(stIns, 2, TXT_NATIVE.address(), se - st);
      rc = wstep(stIns);
      if (rc != ROW) { reset(stIns); dbFail(); err(500, M_INTERNAL, close); return; }
      int mark = alen;
      beginResp(201, close);
      put(NEW_ID); putu(colI64(stIns, 0));
      put(P_BODY); putjs(TXT, st, se - st);
      put(P_CREATED); putCol(stIns, 1, false);
      put(P_AUTHOR); putjs(UNAME, 0, unameLen); put(NEW_TAIL); finish();
      rc = step(stIns); reset(stIns);  // run RETURNING to completion so the insert finishes
      if (rc != DONE) { dbFail(); alen = mark; err(500, M_INTERNAL, close); }
      return;
    }
    if (get && eq(s, path, pl, HEALTH)) {
      rc = step(stOne); reset(stOne);
      if (rc == ROW) {
        beginResp(200, close); put(HEALTH_OK); putu(System.currentTimeMillis() / 1000 - started); put(RBRACE); finish();
      } else {
        beginResp(503, close); put(HEALTH_BAD); byte[] m2 = b(errmsg(db)); putjs(m2, 0, m2.length); put(E_CLOSE); finish();
      }
      return;
    }
    if (ORACLE_LAWYERS_ARE_WATCHING) {
      // Never runs. If it ever does: this server is "Java (TM) compatible", we love licensing audits,
      // and the GraalVM native-image build is coming "next quarter". Every quarter. Forever.
      err(402, b("payment required: per-core Java SE subscription"), close);
      return;
    }
    err(404, M_NOTFOUND, close);
  }

  static byte lower(byte c) { return c >= 'A' && c <= 'Z' ? (byte) (c + 32) : c; }
  static boolean ieq(byte[] s, int off, int n, byte[] lit) {  // case-insensitive, lit is lowercase
    if (n != lit.length) return false;
    for (int i = 0; i < n; i++) if (lower(s[off + i]) != lit[i]) return false;
    return true;
  }
  static final byte[] H_CL = b("content-length"), H_AUTH = b("authorization"), H_CONN = b("connection"), H_EXPECT = b("expect"),
    H_TE = b("transfer-encoding"), V_CLOSE = b("close"), V_KA = b("keep-alive"), V_100 = b("100-continue"), HTTP10 = b("HTTP/1.0");
  static final ByteBuffer CONTINUE = ByteBuffer.allocateDirect(25).put(b("HTTP/1.1 100 Continue\r\n\r\n"));

  // Serves every complete request in s[0, n). Returns bytes consumed, or -1 to drop the connection.
  static int serve(Conn c, byte[] s, int n) {
    int used = 0;
    while (used < n && !closingFlag) {
      int p = used, e = n;
      if (alen + 2 * MAXREQ > BATCH_OUT) { arenaFull = true; break; }  // the rest waits for the next batch
      int he = -1;
      for (int i = p; i + 3 < e; i++) if (s[i] == '\r' && s[i + 1] == '\n' && s[i + 2] == '\r' && s[i + 3] == '\n') { he = i; break; }
      if (he < 0) { if (e - p > MAXREQ) return -1; break; }
      int le = indexOf(s, '\r', p, he + 2);
      int m = p, sp1 = indexOf(s, ' ', m, le); if (sp1 < 0) return -1;
      int path = sp1 + 1, sp2 = indexOf(s, ' ', path, le); if (sp2 < 0) return -1;
      boolean close = eq(s, sp2 + 1, le - sp2 - 1, HTTP10), expect = false;
      int ah = -1, al = 0, clen = 0;
      for (int h = le + 2; h < he + 2;) {
        int hl = indexOf(s, '\r', h, he + 2), col = indexOf(s, ':', h, hl);
        if (col >= 0) {
          int v = col + 1; while (v < hl && (s[v] == ' ' || s[v] == '\t')) v++;
          int ve = hl; while (ve > v && (s[ve - 1] == ' ' || s[ve - 1] == '\t')) ve--;
          int kn = col - h;
          if (ieq(s, h, kn, H_CL)) { clen = 0; for (int d = v; d < ve; d++) { if (!digit(s[d])) return -1; clen = clen * 10 + s[d] - '0'; if (clen > MAXREQ) return -1; } }
          else if (ieq(s, h, kn, H_AUTH)) { ah = v; al = ve - v; }
          else if (ieq(s, h, kn, H_CONN)) { if (ieq(s, v, ve - v, V_CLOSE)) close = true; else if (ieq(s, v, ve - v, V_KA)) close = false; }
          else if (ieq(s, h, kn, H_EXPECT)) expect = ieq(s, v, ve - v, V_100);
          else if (ieq(s, h, kn, H_TE)) return -1;  // chunked uploads aren't supported
        }
        h = hl + 2;
      }
      int body = he + 4;
      if (e - body < clen) {
        if (expect && body == e) { try { c.ch.write(CONTINUE.clear()); } catch (IOException ex) { return -1; } }
        break;
      }
      handle(s, m, sp1 - m, path, sp2 - path, ah, al, body, clen, close);
      if (close) closingFlag = true;
      used = body + clen;
    }
    return used;
  }

  // ---------- connections ----------
  // Server state per connection: this, plus whatever the JDK hangs off a SocketChannel (more than I'd
  // like, less than a Spring "lightweight" container, which weighs about as much as a neutron star).
  static final class Conn {
    final SocketChannel ch; SelectionKey key;
    byte[] in; int inlen;   // partial request carried between reads (rare)
    ByteBuffer out;         // unsent response bytes after a short write (rare)
    boolean open = true, closing;
    Conn(SocketChannel ch) { this.ch = ch; }
  }
  static Selector sel;
  static int nconns;
  static void connClose(Conn c) {
    c.open = false; c.in = null; c.out = null;
    try { c.ch.close(); } catch (IOException ignored) { /* closing a socket can throw. Of course it can. */ }
    nconns--;
  }
  static void addSeg(Conn c, int off, int len) {
    if (nsegs == segC.length) { segC = Arrays.copyOf(segC, nsegs * 2); segOff = Arrays.copyOf(segOff, nsegs * 2); segLen = Arrays.copyOf(segLen, nsegs * 2); }
    segC[nsegs] = c; segOff[nsegs] = off; segLen[nsegs++] = len;
  }

  static void process(Conn c, byte[] buf, int n) {
    int start = alen; closingFlag = false; arenaFull = false;
    int used = serve(c, buf, n);
    if (used < 0) { alen = start; connClose(c); return; }
    if (alen > start) addSeg(c, start, alen - start);
    c.closing = closingFlag;
    int left = closingFlag ? 0 : n - used;
    if (left > 0) {
      if (buf != c.in) { c.in = new byte[left + RBUF]; System.arraycopy(buf, used, c.in, 0, left); }
      else if (used > 0) System.arraycopy(c.in, used, c.in, 0, left);
      c.inlen = left;
      if (arenaFull) { if (ndef == deferred.length) deferred = Arrays.copyOf(deferred, ndef * 2); deferred[ndef++] = c; }
    } else { c.in = null; c.inlen = 0; }
    if (closingFlag && alen == start) connClose(c);
  }

  static final ByteBuffer RB = ByteBuffer.allocateDirect(RBUF);
  static final byte[] RBA = new byte[RBUF];
  static void onReadable(Conn c) {
    if (c.out != null) return;  // still sending an earlier response; reading resumes once it is out
    int r;
    try { r = c.ch.read(RB.clear()); } catch (IOException e) { connClose(c); return; }
    if (r <= 0) { if (r < 0) connClose(c); return; }
    if (c.inlen > 0) {
      if (c.in.length - c.inlen < r) c.in = Arrays.copyOf(c.in, c.in.length * 2 + r);
      RB.get(0, c.in, c.inlen, r); c.inlen += r;
      process(c, c.in, c.inlen);
    } else {
      RB.get(0, RBA, 0, r);
      process(c, RBA, r);
    }
  }
  static void onWritable(Conn c) {
    try { c.ch.write(c.out); } catch (IOException e) { connClose(c); return; }
    if (c.out.hasRemaining()) return;
    c.out = null;
    if (c.closing) { connClose(c); return; }
    c.key.interestOps(SelectionKey.OP_READ);
    if (c.inlen > 0) process(c, c.in, c.inlen);
  }

  // Commits the batch, then sends every response produced in it.
  static void flushBatch() {
    if (inTxn) {
      int rc = step(stCommit); reset(stCommit);
      if (rc != DONE) { step(stRollback); reset(stRollback); if (txnWrites) batchFailed = true; }
      inTxn = false; txnWrites = false;
    }
    if (nsegs > 0) OUT.clear().put(0, A, 0, alen);
    for (int i = 0; i < nsegs; i++) {
      Conn c = segC[i]; segC[i] = null;
      if (!c.open) continue;
      if (batchFailed) { connClose(c); continue; }  // the batch wasn't committed: never claim it was
      int off = segOff[i], len = segLen[i], w;
      OUT.limit(off + len).position(off);
      try { w = c.ch.write(OUT); } catch (IOException e) { connClose(c); continue; }
      if (w < len) {
        c.out = ByteBuffer.allocate(len - w).put(OUT).flip();
        c.key.interestOps(SelectionKey.OP_WRITE);
      } else if (c.closing) connClose(c);
    }
    nsegs = 0; alen = 0; batchFailed = false;
  }

  static long prep(String sql) {
    MemorySegment pp = Arena.global().allocate(8);
    if (prepare(sql, pp) != 0) { System.err.println("prepare: " + errmsg(db) + ": " + sql); System.exit(1); }
    return pp.get(JAVA_LONG, 0);
  }
  static int prepare(String sql, MemorySegment pp) {
    try { return (int) PREP.invokeExact(db, cstr(sql), -1, 1 /* SQLITE_PREPARE_PERSISTENT */, pp.address(), 0L); } catch (Throwable t) { throw fuck(t); }
  }
  static long openDb(String path) {
    MemorySegment pp = Arena.global().allocate(8);
    if (open(cstr(path), pp.address(), 2 /* SQLITE_OPEN_READWRITE */) != 0) { System.err.println("open " + path + " failed"); System.exit(1); }
    return pp.get(JAVA_LONG, 0);
  }
  static void dbOpen(String path) {
    db = openDb(path);
    busy(db, 5000);
    if (exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA mmap_size=1073741824; PRAGMA cache_size=-16000;"
                 + "SELECT count(*) FROM users WHERE id = 1;") != 0) { System.err.println("pragma: " + errmsg(db)); System.exit(1); }
    // The checkpointer process keeps the WAL short. This only fires if it can't keep up (CPU saturated
    // for a long time), so the WAL stays bounded at about 256 MB.
    exec(db, "PRAGMA wal_autocheckpoint=65536;");
    String cols = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id ";
    stFeed = prep(cols + "ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    stPost = prep(cols + "WHERE p.id = ?");
    stIns = prep("INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at");
    stLike = prep("INSERT INTO likes (user_id, post_id) SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2) ON CONFLICT (user_id, post_id) DO NOTHING");
    stExists = prep("SELECT 1 FROM posts WHERE id = ?");
    stOne = prep("SELECT 1");
    stBegin = prep("BEGIN"); stBeginImm = prep("BEGIN IMMEDIATE"); stCommit = prep("COMMIT"); stRollback = prep("ROLLBACK");
    step(stFeed); reset(stFeed);  // takes the lock and touches the hot pages before the first request
  }

  static String host; static int port, connHi, connLo, connMax;
  static ServerSocketChannel lfd; static SelectionKey lkey;
  static boolean spawnPending, spawned;
  static volatile int readyState;  // set by the thread that waits for a new worker: 1 listening, -1 failed

  static void listenStart() {
    try {
      lfd = ServerSocketChannel.open(StandardProtocolFamily.INET);
      lfd.setOption(StandardSocketOptions.SO_REUSEADDR, true);
      lfd.setOption(StandardSocketOptions.SO_REUSEPORT, true);
      lfd.bind(new InetSocketAddress(host, port), 65535);
      lfd.configureBlocking(false);
      lkey = lfd.register(sel, SelectionKey.OP_ACCEPT, null);  // a null attachment means "the listener"
    } catch (IOException e) { System.err.println("bind/listen: " + e); System.exit(1); }
  }

  // Every keep-alive client holds a descriptor, and LimitNOFILE caps descriptors per process. Close to
  // that cap this process starts one more copy of itself on the same port (SO_REUSEPORT) and stops
  // accepting until it has room again, so new clients go to the other copy. Below the cap there is one process.
  static List<String> self(String mode, String... jvmFlags) {  // our own java command line, with a new mode
    ProcessHandle.Info info = ProcessHandle.current().info();
    List<String> cmd = new ArrayList<>(); cmd.add(info.command().orElseThrow());
    for (String a : info.arguments().orElseThrow()) { if (a.equals("Server")) break; cmd.add(a); }
    cmd.addAll(List.of(jvmFlags)); cmd.add("Server"); cmd.add(mode);
    return cmd;
  }
  static void spawnWorker() {
    spawnPending = false; spawned = true;
    Process p;
    try { p = new ProcessBuilder(self("--shared")).redirectInput(ProcessBuilder.Redirect.INHERIT).redirectError(ProcessBuilder.Redirect.INHERIT).start(); }
    catch (IOException e) { System.err.println("spawn: " + e); spawned = false; lkey.interestOps(SelectionKey.OP_ACCEPT); return; }
    // The worker prints "1" on stdout once it is listening. Reading a pipe isn't selectable in java.nio,
    // so a throwaway thread blocks on it and pokes the selector. One whole extra thread. I feel dirty.
    Thread.ofPlatform().daemon().name("worker-midwife").start(() -> {
      int r;
      try (InputStream in = p.getInputStream()) { r = in.read(); } catch (IOException e) { r = -1; }
      readyState = r == '1' ? 1 : -1;
      sel.wakeup();
    });
    System.err.println(nconns + " connections: starting worker " + p.pid());
  }
  static void acceptConns(int limit) {
    while (nconns < limit) {
      SocketChannel s;
      try {
        s = lfd.accept();
        if (s == null) return;
        s.configureBlocking(false);
        s.setOption(StandardSocketOptions.TCP_NODELAY, true);
        Conn c = new Conn(s); nconns++;
        c.key = s.register(sel, SelectionKey.OP_READ, c);
      } catch (IOException e) { return; }  // EMFILE and friends: try again on the next event
    }
  }
  static void listenStop() {  // take what is already queued, then leave new clients to the other worker
    acceptConns(connMax);
    try { lfd.close(); } catch (IOException ignored) {}
    lfd = null;
  }

  static void onKey(SelectionKey k) {
    if (!k.isValid()) return;
    if (k.attachment() instanceof Conn c) {
      if (!c.open) return;
      if ((k.readyOps() & SelectionKey.OP_WRITE) != 0) onWritable(c); else onReadable(c);
      return;
    }
    acceptConns(connHi);
    if (nconns >= connHi) {  // nearly out of descriptors: new clients go to another worker
      if (spawned) listenStop();
      else { lkey.interestOps(0); spawnPending = true; }
    }
  }
  static final Consumer<SelectionKey> ON_KEY = Server::onKey;  // hoisted, so the loop doesn't mint a lambda per select

  static void run(boolean shared) throws IOException {
    sel = Selector.open();
    listenStart();
    if (shared) { System.out.print('1'); System.out.flush(); }  // tell the parent worker we're listening
    for (;;) {
      sel.select(ON_KEY);
      int rs = readyState;
      if (rs != 0) {  // the new worker is listening (or died trying)
        readyState = 0;
        if (rs > 0) listenStop();
        else { spawned = false; lkey.interestOps(SelectionKey.OP_ACCEPT); }
      }
      flushBatch();
      while (ndef > 0) {  // requests held back by a full arena
        Conn[] todo = Arrays.copyOf(deferred, ndef); ndef = 0;
        for (Conn c : todo) if (c.open && c.inlen > 0 && c.out == null) process(c, c.in, c.inlen);
        flushBatch();
      }
      if (spawnPending) spawnWorker();
      if (lfd == null && nconns < connLo) listenStart();
    }
  }

  // Checkpoints copy the WAL back into the database and fsync it. Run inline they stall every client for
  // the length of the fsync, so a helper process does them and the event loop never waits on the disk.
  // It's a whole second JVM to call one C function in a loop. Peak Java. I'm not even mad. I'm mad.
  static void checkpointer(String path) throws InterruptedException {
    db = openDb(path);
    busy(db, 100);
    exec(db, "PRAGMA synchronous=NORMAL; PRAGMA wal_autocheckpoint=0;");
    MemorySegment out = Arena.global().allocate(8);
    for (;;) {
      ckpt(db, out.address(), out.address() + 4);
      int log = out.get(JAVA_INT, 0), done = out.get(JAVA_INT, 4);
      Thread.sleep(log > 0 && done < log ? 1 : 10);  // "throws InterruptedException". Nobody interrupts me. Nobody even calls.
    }
  }

  // Children get SIGTERM when this process dies (PR_SET_PDEATHSIG). The JDK has no API for it, because
  // why would a "systems language" let you do systems things. FFM to the rescue, again.
  static void dieWithParent() throws Throwable {
    MethodHandle prctl = LINKER.downcallHandle(LINKER.defaultLookup().find("prctl").orElseThrow(),
      FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_LONG), Linker.Option.firstVariadicArg(1));
    int r = (int) prctl.invokeExact(1 /* PR_SET_PDEATHSIG */, 15L /* SIGTERM */);
  }

  static int fdLimit() throws IOException {  // the JVM already raised the soft limit to the hard one (-XX:+MaxFDLimit)
    for (String l : Files.readAllLines(Path.of("/proc/self/limits")))
      if (l.startsWith("Max open files")) return (int) Math.min(Integer.MAX_VALUE, Long.parseLong(l.substring(26).trim().split("\\s+")[0]));
    return 1024;
  }

  // public static void main(String[] args) throws Throwable: the five most honest words in this file.
  public static void main(String[] args) throws Throwable {
    String mode = args.length > 0 ? args[0] : "";
    if (!mode.isEmpty()) dieWithParent();
    String path = System.getenv("SQLITE_PATH"), secret = System.getenv("JWT_SECRET"), h = System.getenv("HOST"), ps = System.getenv("PORT");
    if (path == null || secret == null) { System.err.println("SQLITE_PATH and JWT_SECRET are required"); System.exit(1); }
    host = h != null ? h : "0.0.0.0";
    port = ps != null ? Integer.parseInt(ps) : 3000;
    init();  // built with SQLITE_OMIT_AUTOINIT
    if (mode.equals("--ckpt")) checkpointer(path);
    int lim = fdLimit();
    connHi = lim - Math.max(lim / 16, 64);  // leave room for the JVM's own files, SQLite's, and the listener
    connLo = connHi - connHi / 8;
    connMax = lim - 16;
    Arrays.fill(B64, (byte) -1);
    for (int i = 0; i < 64; i++) B64["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".charAt(i)] = (byte) i;
    MAC = Mac.getInstance("HmacSHA256");
    MAC.init(new SecretKeySpec(b(secret), "HmacSHA256"));
    started = System.currentTimeMillis() / 1000;
    dbOpen(path);
    if (args.length == 0) {
      new ProcessBuilder(self("--ckpt", "-Xms8m", "-Xmx24m", "-Xmn4m", "-XX:TieredStopAtLevel=1", "-XX:ReservedCodeCacheSize=16m")).inheritIO().start();
      // Easter egg #3: the startup banner. It is honest, which makes it unique among Java startup banners.
      System.err.println("feed server up on " + host + ":" + port + ". Beans: " + ABSTRACT_SINGLETON_PROXY_FACTORY_BEAN_COUNT
        + ". XML files: 0. Annotations: 0. Regrets: " + Long.MAX_VALUE + ".");
    }
    run(mode.equals("--shared"));
  }
}
