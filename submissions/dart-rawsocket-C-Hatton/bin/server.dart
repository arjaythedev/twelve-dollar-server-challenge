// $12 server challenge: a single-isolate Dart server that speaks HTTP/1.1 itself over
// dart:io ServerSocket (no HttpServer), backed by SQLite through FFI with reused
// prepared statements.
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:sqlite3/open.dart';
import 'package:sqlite3/sqlite3.dart';

late final Database db;
late final PreparedStatement stFeed;
late final PreparedStatement stPost;
late final PreparedStatement stInsertPost;
late final PreparedStatement stPostExists;
late final PreparedStatement stInsertLike;
late final PreparedStatement stSelect1;
late final Hmac hmac;
late final DateTime startedAt;

// Text columns are read as BLOBs: their raw UTF-8 bytes are copied straight into the
// response, skipping a UTF-8 decode to String and a re-encode.
const _postCols = 'SELECT p.id, CAST(p.body AS BLOB), CAST(p.created_at AS BLOB), '
    'CAST(u.username AS BLOB), '
    '(SELECT count(*) FROM likes l WHERE l.post_id = p.id) '
    'FROM posts p JOIN users u ON u.id = p.user_id ';

void main() async {
  startedAt = DateTime.now();
  open.overrideFor(OperatingSystem.linux, () {
    try {
      return DynamicLibrary.open('libsqlite3.so');
    } catch (_) {
      return DynamicLibrary.open('libsqlite3.so.0');
    }
  });

  final env = Platform.environment;
  final path = env['SQLITE_PATH'] ?? 'feed.db';
  hmac = Hmac(sha256, utf8.encode(env['JWT_SECRET'] ?? ''));
  final host = env['HOST'] ?? '127.0.0.1';
  final port = int.parse(env['PORT'] ?? '3000');

  db = sqlite3.open(path, mutex: false); // single isolate: no SQLite mutexes needed
  stdout.writeln('sqlite ${sqlite3.version}');
  db.execute('PRAGMA journal_mode = WAL');
  db.execute('PRAGMA synchronous = NORMAL');
  db.execute('PRAGMA busy_timeout = 5000');
  db.execute('PRAGMA cache_size = -262144'); // 256 MB page cache
  db.execute('PRAGMA mmap_size = 536870912');
  db.execute('PRAGMA temp_store = MEMORY');
  stFeed = db.prepare('$_postCols ORDER BY p.created_at DESC, p.id DESC LIMIT 20',
      persistent: true);
  stPost = db.prepare('$_postCols WHERE p.id = ?', persistent: true);
  stInsertPost = db.prepare(
      'INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at',
      persistent: true);
  stPostExists =
      db.prepare('SELECT 1 FROM posts WHERE id = ?', persistent: true);
  stInsertLike = db.prepare(
      'INSERT INTO likes (user_id, post_id) VALUES (?, ?) '
      'ON CONFLICT (user_id, post_id) DO NOTHING',
      persistent: true);
  stSelect1 = db.prepare('SELECT 1', persistent: true);

  final server = await ServerSocket.bind(host, port, backlog: 4096, shared: false);
  stdout.writeln('listening on $host:$port');
  server.listen(_onConnection);
}

void _onConnection(Socket socket) {
  socket.setOption(SocketOption.tcpNoDelay, true);
  final conn = _Conn(socket);
  socket.done.then((_) {}, onError: (_) {}); // peer resets must not crash the isolate
  socket.listen(conn.onData,
      onError: (_) => conn.close(), onDone: conn.close, cancelOnError: true);
}

// ---------------------------------------------------------------------------
// HTTP/1.1 parsing

final _crlfcrlf = [13, 10, 13, 10];

class _Conn {
  final Socket socket;
  Uint8List buf = Uint8List(0);
  bool closed = false;
  _Conn(this.socket);

  void close() {
    if (closed) return;
    closed = true;
    socket.destroy();
  }

  void onData(Uint8List data) {
    if (buf.isEmpty) {
      buf = data;
    } else {
      final nb = Uint8List(buf.length + data.length);
      nb.setRange(0, buf.length, buf);
      nb.setRange(buf.length, nb.length, data);
      buf = nb;
    }
    final out = BytesBuilder(copy: false);
    var pos = 0;
    while (true) {
      final hdrEnd = _indexOf(buf, pos);
      if (hdrEnd < 0) {
        if (buf.length - pos > 65536) return _bad();
        break;
      }
      final req = _parseHead(buf, pos, hdrEnd);
      if (req == null) return _bad();
      final bodyStart = hdrEnd + 4;
      if (req.contentLength > 1 << 20) return _bad();
      if (buf.length - bodyStart < req.contentLength) break;
      final body = Uint8List.sublistView(buf, bodyStart, bodyStart + req.contentLength);
      pos = bodyStart + req.contentLength;
      out.add(_handle(req, body));
      if (req.close) {
        if (out.isNotEmpty) socket.add(out.takeBytes());
        socket.flush().then((_) => close(), onError: (_) => close());
        closed = true;
        return;
      }
    }
    buf = pos >= buf.length ? Uint8List(0) : Uint8List.fromList(buf.sublist(pos));
    if (out.isNotEmpty) socket.add(out.takeBytes());
  }

  void _bad() {
    socket.add(_response(400, '{"error":"bad request"}', close: true));
    socket.flush().then((_) => close(), onError: (_) => close());
    closed = true;
  }
}

int _indexOf(Uint8List b, int start) {
  final n = b.length - 3;
  for (var i = start; i < n; i++) {
    if (b[i] == 13 && b[i + 1] == 10 && b[i + 2] == 13 && b[i + 3] == 10) {
      return i;
    }
  }
  return -1;
}

class _Req {
  final String method;
  final String path;
  final String? auth;
  final int contentLength;
  final bool close;
  _Req(this.method, this.path, this.auth, this.contentLength, this.close);
}

_Req? _parseHead(Uint8List b, int start, int end) {
  // request line
  var i = start;
  while (i < end && b[i] != 32) i++;
  if (i >= end) return null;
  final method = String.fromCharCodes(b, start, i);
  final ps = ++i;
  while (i < end && b[i] != 32 && b[i] != 63 /* ? */) i++;
  final path = String.fromCharCodes(b, ps, i);
  while (i < end && b[i] != 13) i++;
  final http10 = i >= 8 && b[i - 1] == 48 && b[i - 3] == 49; // HTTP/1.0
  i += 2;
  String? auth;
  var cl = 0;
  var close = http10;
  while (i < end) {
    final ls = i;
    while (i < end && b[i] != 13) i++;
    final le = i;
    i += 2;
    var c = ls;
    while (c < le && b[c] != 58) c++;
    if (c >= le) continue;
    final nameLen = c - ls;
    var vs = c + 1;
    while (vs < le && (b[vs] == 32 || b[vs] == 9)) vs++;
    var ve = le;
    while (ve > vs && (b[ve - 1] == 32 || b[ve - 1] == 9)) ve--;
    final f = b[ls] | 0x20;
    if (nameLen == 14 && f == 0x63 && _ieq(b, ls, 'content-length')) {
      cl = 0;
      for (var k = vs; k < ve; k++) {
        final d = b[k] - 48;
        if (d < 0 || d > 9) return null;
        cl = cl * 10 + d;
      }
    } else if (nameLen == 13 && f == 0x61 && _ieq(b, ls, 'authorization')) {
      auth = utf8.decode(Uint8List.sublistView(b, vs, ve), allowMalformed: true);
    } else if (nameLen == 10 && f == 0x63 && _ieq(b, ls, 'connection')) {
      final v = String.fromCharCodes(b, vs, ve).toLowerCase();
      if (v == 'close') close = true;
      if (v == 'keep-alive') close = false;
    } else if (nameLen == 17 && f == 0x74 && _ieq(b, ls, 'transfer-encoding')) {
      return null; // chunked request bodies are not supported
    }
  }
  return _Req(method, path, auth, cl, close);
}

bool _ieq(Uint8List b, int s, String name) {
  for (var k = 0; k < name.length; k++) {
    if ((b[s + k] | 0x20) != name.codeUnitAt(k)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Responses

const _reasons = {
  200: 'OK',
  201: 'Created',
  400: 'Bad Request',
  401: 'Unauthorized',
  404: 'Not Found',
  500: 'Internal Server Error',
  503: 'Service Unavailable',
};

Uint8List _response(int status, String json, {bool close = false}) {
  final body = utf8.encode(json);
  final head = 'HTTP/1.1 $status ${_reasons[status]}\r\n'
      'Content-Type: application/json\r\n'
      'Content-Length: ${body.length}\r\n'
      '${close ? 'Connection: close\r\n' : ''}'
      '\r\n';
  final out = Uint8List(head.length + body.length);
  for (var k = 0; k < head.length; k++) {
    out[k] = head.codeUnitAt(k);
  }
  out.setRange(head.length, out.length, body);
  return out;
}

Uint8List _err(int status, String msg) => _response(status, '{"error":"$msg"}');

final _notFound = _err(404, 'not found');
final _invalidId = _err(400, 'invalid post id');
final _postNotFound = _err(404, 'post not found');
final _missingBearer = _err(401, 'missing bearer token');
final _invalidToken = _err(401, 'invalid or expired token');
final _invalidPayload = _err(401, 'invalid token payload');
final _malformed = _err(400, 'malformed JSON body');
final _bodyRequired = _err(400, 'body is required');
final _bodyTooLong = _err(400, 'body must be at most 500 characters');
final _internal = _err(500, 'internal server error');

// ---------------------------------------------------------------------------
// Routing

Uint8List _handle(_Req req, Uint8List body) {
  try {
    final p = req.path;
    final get = req.method == 'GET';
    final post = req.method == 'POST';
    if (get && p == '/feed') return _feed();
    if (p.startsWith('/posts/')) {
      final rest = p.substring(7);
      if (rest.endsWith('/like')) {
        if (!post) return _notFound;
        final user = _auth(req.auth);
        if (user is Uint8List) return user;
        final id = _parseId(rest.substring(0, rest.length - 5));
        if (id < 0) return _invalidId;
        return _like(user as _User, id);
      }
      if (rest.contains('/')) return _notFound;
      if (!get) return _notFound;
      final id = _parseId(rest);
      if (id < 0) return _invalidId;
      return _getPost(id);
    }
    if (post && p == '/posts') {
      final user = _auth(req.auth);
      if (user is Uint8List) return user;
      return _createPost(user as _User, body);
    }
    if (get && p == '/health') return _health();
    return _notFound;
  } catch (e) {
    stderr.writeln('error: $e');
    return _internal;
  }
}

int _parseId(String s) {
  if (s.isEmpty || s.length > 15) return -1;
  var n = 0;
  for (var k = 0; k < s.length; k++) {
    final d = s.codeUnitAt(k) - 48;
    if (d < 0 || d > 9) return -1;
    n = n * 10 + d;
  }
  return n > 0 ? n : -1;
}

// ---------------------------------------------------------------------------
// Handlers

/// Reusable scratch buffer that JSON bodies are serialized into as bytes.
class _Out {
  Uint8List b = Uint8List(1 << 16);
  int n = 0;

  void _grow(int extra) {
    if (n + extra <= b.length) return;
    final nb = Uint8List((n + extra) * 2);
    nb.setRange(0, n, b);
    b = nb;
  }

  void ascii(String s) {
    _grow(s.length);
    for (var k = 0; k < s.length; k++) {
      b[n++] = s.codeUnitAt(k);
    }
  }

  void integer(int v) => ascii(v.toString());

  /// Writes UTF-8 bytes as a JSON string, escaping only `"`, `\` and control characters.
  void str(Uint8List s) {
    _grow(s.length * 6 + 2);
    final b = this.b;
    var n = this.n;
    b[n++] = 34;
    for (var k = 0; k < s.length; k++) {
      final c = s[k];
      if (c >= 0x20 && c != 34 && c != 92) {
        b[n++] = c;
      } else if (c == 34 || c == 92) {
        b[n++] = 92;
        b[n++] = c;
      } else {
        const hex = '0123456789abcdef';
        b[n++] = 92;
        b[n++] = 117; // u
        b[n++] = 48;
        b[n++] = 48;
        b[n++] = hex.codeUnitAt(c >> 4);
        b[n++] = hex.codeUnitAt(c & 15);
      }
    }
    b[n++] = 34;
    this.n = n;
  }

  /// Builds the full HTTP response around the buffered body and resets the buffer.
  Uint8List finish(int status) {
    final head = 'HTTP/1.1 $status ${_reasons[status]}\r\n'
        'Content-Type: application/json\r\n'
        'Content-Length: $n\r\n\r\n';
    final out = Uint8List(head.length + n);
    for (var k = 0; k < head.length; k++) {
      out[k] = head.codeUnitAt(k);
    }
    out.setRange(head.length, out.length, b);
    n = 0;
    return out;
  }
}

final _out = _Out();

void _writePost(_Out o, Row r) {
  o.ascii('{"id":');
  o.integer(r.columnAt(0) as int);
  o.ascii(',"body":');
  o.str(r.columnAt(1) as Uint8List);
  o.ascii(',"created_at":');
  o.str(r.columnAt(2) as Uint8List);
  o.ascii(',"author":');
  o.str(r.columnAt(3) as Uint8List);
  o.ascii(',"like_count":');
  o.integer(r.columnAt(4) as int);
  o.b[o.n++] = 125; // }
}

Uint8List _feed() {
  final rows = stFeed.select();
  final o = _out..n = 0;
  o.ascii('{"posts":[');
  var first = true;
  for (final r in rows) {
    if (!first) o.ascii(',');
    first = false;
    _writePost(o, r);
  }
  o.ascii(']}');
  return o.finish(200);
}

Uint8List _getPost(int id) {
  final rows = stPost.select([id]);
  if (rows.isEmpty) return _postNotFound;
  final o = _out..n = 0;
  o.ascii('{"post":');
  _writePost(o, rows.first);
  o.ascii('}');
  return o.finish(200);
}

Uint8List _health() {
  try {
    stSelect1.select();
  } catch (e) {
    return _response(503,
        '{"status":"degraded","db":"unreachable","error":${jsonEncode(e.toString())}}');
  }
  final up = DateTime.now().difference(startedAt).inSeconds;
  return _response(200, '{"status":"ok","db":"ok","uptime_s":$up}');
}

Uint8List _createPost(_User user, Uint8List raw) {
  Object? parsed;
  try {
    parsed = jsonDecode(utf8.decode(raw));
  } catch (_) {
    return _malformed;
  }
  if (parsed is! Map) return _bodyRequired;
  final b = parsed['body'];
  if (b is! String) return _bodyRequired;
  final text = b.trim();
  if (text.isEmpty) return _bodyRequired;
  if (text.length > 500 && text.runes.length > 500) return _bodyTooLong;
  final rows = stInsertPost.select([user.id, text]);
  final r = rows.first;
  return _response(
      201,
      '{"post":{"id":${r.columnAt(0)},"body":${jsonEncode(text)},'
      '"created_at":${jsonEncode(r.columnAt(1))},'
      '"author":${jsonEncode(user.name)},"like_count":0}}');
}

Uint8List _like(_User user, int postId) {
  if (stPostExists.select([postId]).isEmpty) return _postNotFound;
  stInsertLike.execute([user.id, postId]);
  final inserted = db.updatedRows > 0;
  return _response(inserted ? 201 : 200,
      '{"liked":true,"already_liked":${!inserted},"post_id":$postId}');
}

// ---------------------------------------------------------------------------
// JWT (HS256), verified on every request

class _User {
  final int id;
  final String name;
  _User(this.id, this.name);
}

final _subRe = RegExp(r'^[1-9][0-9]*$');

/// Returns a [_User] or an error response.
Object _auth(String? header) {
  if (header == null || !header.startsWith('Bearer ')) return _missingBearer;
  final token = header.substring(7).trim();
  final d1 = token.indexOf('.');
  final d2 = d1 < 0 ? -1 : token.indexOf('.', d1 + 1);
  if (d1 < 0 || d2 < 0 || token.indexOf('.', d2 + 1) >= 0) return _invalidToken;
  Map header0, payload;
  List<int> sig;
  try {
    final h = jsonDecode(utf8.decode(_b64url(token.substring(0, d1))));
    final p = jsonDecode(utf8.decode(_b64url(token.substring(d1 + 1, d2))));
    if (h is! Map || p is! Map) return _invalidToken;
    header0 = h;
    payload = p;
    sig = _b64url(token.substring(d2 + 1));
  } catch (_) {
    return _invalidToken;
  }
  if (header0['alg'] != 'HS256') return _invalidToken;
  final expect = hmac.convert(ascii.encode(token.substring(0, d2))).bytes;
  if (sig.length != expect.length) return _invalidToken;
  var diff = 0;
  for (var k = 0; k < sig.length; k++) {
    diff |= sig[k] ^ expect[k];
  }
  if (diff != 0) return _invalidToken;
  final exp = payload['exp'];
  if (exp != null) {
    if (exp is! num) return _invalidToken;
    if (DateTime.now().millisecondsSinceEpoch / 1000 >= exp) return _invalidToken;
  }
  final sub = payload['sub'];
  final name = payload['username'];
  if (sub is! String || name is! String || !_subRe.hasMatch(sub) || sub.length > 15) {
    return _invalidPayload;
  }
  return _User(int.parse(sub), name);
}

List<int> _b64url(String s) {
  if (s.contains('=')) throw const FormatException();
  return base64Url.decode(base64Url.normalize(s));
}
