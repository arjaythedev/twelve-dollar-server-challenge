<?php
/**
 * The $12 server challenge API: Swoole's C HTTP server + SQLite, tuned for one vCPU.
 *
 * One worker process in SWOOLE_BASE mode accepts, parses and answers every connection on a single
 * epoll loop (no reactor->worker IPC). SQLite is called inline: every query is an index lookup that
 * takes microseconds, so a thread hop would cost more than it saves on one core.
 *
 * A second process (the "checkpointer") owns WAL checkpoints, so their fsyncs never stall the loop.
 */
declare(strict_types=1);

use Swoole\Http\Request;
use Swoole\Http\Response;
use Swoole\Http\Server;
use Swoole\Process;

$SQLITE_PATH = getenv('SQLITE_PATH') ?: throw new RuntimeException('SQLITE_PATH is not set');
$JWT_SECRET = getenv('JWT_SECRET') ?: throw new RuntimeException('JWT_SECRET is not set');
$HOST = getenv('HOST') ?: '127.0.0.1';
$PORT = (int) (getenv('PORT') ?: 3000);
$START = time();

const JSON_CT = 'application/json';
const E_NOT_FOUND = '{"error":"not found"}';
const E_BAD_ID = '{"error":"invalid post id"}';
const E_POST_NOT_FOUND = '{"error":"post not found"}';
const E_MISSING_TOKEN = '{"error":"missing bearer token"}';
const E_BAD_TOKEN = '{"error":"invalid or expired token"}';
const E_BAD_PAYLOAD = '{"error":"invalid token payload"}';
const E_MALFORMED = '{"error":"malformed JSON body"}';
const E_BODY_REQUIRED = '{"error":"body is required"}';
const E_BODY_TOO_LONG = '{"error":"body must be at most 500 characters"}';
const E_INTERNAL = '{"error":"internal server error"}';

// The reference post query. (Letting SQLite render the JSON with json_object/group_concat was
// measured too: ~12% more CPU per request than fetching rows and concatenating in PHP.)
const POST_SELECT = 'SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)'
    . ' FROM posts p JOIN users u ON u.id = p.user_id';
const FEED_SQL = POST_SELECT . ' ORDER BY p.created_at DESC, p.id DESC LIMIT 20';
const POST_SQL = POST_SELECT . ' WHERE p.id = ?';
const JSON_FLAGS = JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_INVALID_UTF8_SUBSTITUTE;
const INSERT_POST_SQL = 'INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at';
// Inserts only if the post exists; 0 changes means "already liked" or "no such post".
const INSERT_LIKE_SQL = 'INSERT INTO likes (user_id, post_id) SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?)'
    . ' ON CONFLICT (user_id, post_id) DO NOTHING';
const POST_EXISTS_SQL = 'SELECT 1 FROM posts WHERE id = ?';

// PDO, not ext-sqlite3: SQLite3Stmt::execute() steps the statement once, resets it, and fetchArray()
// then runs it again, so every SELECT does its first step twice and INSERT ... RETURNING inserts twice.
function open_db(string $path): PDO
{
    $db = new PDO('sqlite:' . $path, null, null, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_TIMEOUT => 5, // busy timeout, seconds
    ]);
    $db->exec('PRAGMA journal_mode = WAL');
    $db->exec('PRAGMA synchronous = NORMAL'); // rule 6: WAL + NORMAL
    $db->exec('PRAGMA mmap_size = 536870912'); // read pages straight from the OS page cache
    $db->exec('PRAGMA cache_size = -32768'); // 32 MiB
    $db->exec('PRAGMA temp_store = MEMORY');
    return $db;
}

/** The checkpointer process: WAL checkpoints (and their fsyncs) happen here, off the event loop. */
function checkpointer(string $path): void
{
    $db = open_db($path);
    // Fault the hot indexes into the OS page cache (shared with the worker's mmap). Not an
    // application cache: the worker still reads everything through SQLite on every request.
    $db->query('SELECT count(*) FROM likes INDEXED BY likes_post_id_idx')->fetchAll();
    $db->query('SELECT count(*) FROM posts INDEXED BY posts_created_at_id_idx')->fetchAll();
    $db->query('SELECT count(*) FROM users')->fetchAll();
    while (true) {
        sleep(1);
        try {
            $r = $db->query('PRAGMA wal_checkpoint(PASSIVE)')->fetchAll(PDO::FETCH_NUM)[0];
            // [busy, wal frames, checkpointed frames]: once the WAL is big and fully copied back,
            // truncate it so it doesn't keep growing.
            if ($r[1] > 16384 && $r[1] === $r[2]) {
                $db->exec('PRAGMA wal_checkpoint(TRUNCATE)');
            }
        } catch (Throwable $e) {
            fwrite(STDERR, 'checkpoint: ' . $e->getMessage() . "\n");
        }
    }
}

/** Returns [user_id, username] or an error body for a 401. */
function authenticate(Request $req, string $secret): array|string
{
    $auth = $req->header['authorization'] ?? null;
    if ($auth === null || !str_starts_with($auth, 'Bearer ')) return E_MISSING_TOKEN;
    $parts = explode('.', substr($auth, 7));
    if (count($parts) !== 3) return E_BAD_TOKEN;
    [$h, $p, $s] = $parts;
    $header = json_decode((string) base64_decode(strtr($h, '-_', '+/'), true), true);
    if (!is_array($header) || ($header['alg'] ?? null) !== 'HS256') return E_BAD_TOKEN;
    $sig = rtrim(strtr(base64_encode(hash_hmac('sha256', "$h.$p", $secret, true)), '+/', '-_'), '=');
    if (!hash_equals($sig, $s)) return E_BAD_TOKEN;
    $payload = json_decode((string) base64_decode(strtr($p, '-_', '+/'), true), true);
    if (!is_array($payload)) return E_BAD_TOKEN;
    $now = time();
    if (isset($payload['exp']) && (!is_int($payload['exp']) && !is_float($payload['exp']) || $payload['exp'] <= $now)) return E_BAD_TOKEN;
    if (isset($payload['nbf']) && (!is_int($payload['nbf']) && !is_float($payload['nbf']) || $payload['nbf'] > $now)) return E_BAD_TOKEN;
    $sub = $payload['sub'] ?? null;
    $name = $payload['username'] ?? null;
    if (!is_string($sub) || !ctype_digit($sub) || $sub[0] === '0' || strlen($sub) > 18 || !is_string($name)) return E_BAD_PAYLOAD;
    return [(int) $sub, $name];
}

/** [id, body, created_at, author, like_count] -> post object; created_at is stored in wire format. */
function post_json(array $r): string
{
    return '{"id":' . $r[0] . ',"body":' . json_encode($r[1], JSON_FLAGS) . ',"created_at":"' . $r[2]
        . '","author":' . json_encode($r[3], JSON_FLAGS) . ',"like_count":' . $r[4] . '}';
}

/** A positive integer in plain decimal digits, else 0. */
function parse_id(string $raw): int
{
    return ctype_digit($raw) && strlen($raw) <= 18 ? (int) $raw : 0;
}

$server = new Server($HOST, $PORT, SWOOLE_BASE);
$server->set([
    'worker_num' => (int) (getenv('WORKERS') ?: 1),
    'reactor_num' => 1,
    'enable_coroutine' => false, // every handler is synchronous; skip the per-request coroutine
    'max_connection' => 65535,
    'backlog' => 8192,
    'open_tcp_nodelay' => true,
    'http_parse_post' => false,
    'http_parse_cookie' => false,
    'http_parse_files' => false,
    'http_compression' => false,
    'package_max_length' => 65536,
    'log_level' => SWOOLE_LOG_WARNING,
    // no heartbeat: idle keep-alive connections stay open (the spec wants >= 65 s)
]);

$server->addProcess(new Process(function () use ($SQLITE_PATH) {
    cli_set_process_title('feed-checkpointer');
    checkpointer($SQLITE_PATH);
}, false, 0, false));

/** Per-worker SQLite connection and prepared statements, opened after the fork in workerStart. */
final class Db
{
    public static PDO $db;
    public static PDOStatement $health, $feed, $post, $insertPost, $insertLike, $postExists;

    public static function open(string $path): void
    {
        self::$db = $db = open_db($path);
        $db->exec('PRAGMA wal_autocheckpoint = 0'); // the checkpointer process does it
        self::$health = $db->prepare('SELECT 1');
        self::$feed = $db->prepare(FEED_SQL);
        self::$post = $db->prepare(POST_SQL);
        self::$insertPost = $db->prepare(INSERT_POST_SQL);
        self::$insertLike = $db->prepare(INSERT_LIKE_SQL);
        self::$postExists = $db->prepare(POST_EXISTS_SQL);
    }
}

/** Inserts a like; returns [status, body]. */
function like_write(int $userId, int $id): array
{
    $s = Db::$insertLike;
    $s->bindValue(1, $userId, PDO::PARAM_INT);
    $s->bindValue(2, $id, PDO::PARAM_INT);
    $s->bindValue(3, $id, PDO::PARAM_INT);
    $s->execute();
    $inserted = $s->rowCount();
    $s->closeCursor();
    if ($inserted === 1) return [201, '{"liked":true,"already_liked":false,"post_id":' . $id . '}'];
    $s = Db::$postExists;
    $s->bindValue(1, $id, PDO::PARAM_INT);
    $s->execute();
    $exists = $s->fetch(PDO::FETCH_NUM);
    $s->closeCursor();
    return $exists === false ? [404, E_POST_NOT_FOUND] : [200, '{"liked":true,"already_liked":true,"post_id":' . $id . '}'];
}

/** Inserts a post; returns [status, body]. */
function post_write(array $user, string $text): array
{
    $s = Db::$insertPost;
    $s->bindValue(1, $user[0], PDO::PARAM_INT);
    $s->bindValue(2, $text, PDO::PARAM_STR);
    $s->execute();
    $row = $s->fetch(PDO::FETCH_NUM);
    $s->closeCursor();
    return [201, '{"post":' . post_json([$row[0], $text, $row[1], $user[1], 0]) . '}'];
}

/**
 * Group commit: writes that arrive in the same event-loop iteration are queued and run in one
 * transaction at the end of it (Event::defer); each response is sent only after the COMMIT.
 */
final class Writes
{
    private static array $queue = [];

    public static function submit(Response $res, string $fn, mixed $a, mixed $b): void
    {
        if (!self::$queue) Swoole\Event::defer([self::class, 'flush']);
        self::$queue[] = [$res, $fn, $a, $b];
    }

    public static function flush(): void
    {
        $queue = self::$queue;
        self::$queue = [];
        $out = [];
        try {
            Db::$db->exec('BEGIN IMMEDIATE');
            foreach ($queue as [$res, $fn, $a, $b]) $out[] = $fn($a, $b);
            Db::$db->exec('COMMIT');
        } catch (Throwable $e) {
            fwrite(STDERR, $e . "\n");
            foreach ([Db::$insertPost, Db::$insertLike, Db::$postExists] as $stmt) $stmt->closeCursor();
            try { Db::$db->exec('ROLLBACK'); } catch (Throwable) {} // may already be rolled back
            $out = array_fill(0, count($queue), [500, E_INTERNAL]);
        }
        foreach ($queue as $i => [$res]) {
            $res->status($out[$i][0]);
            $res->end($out[$i][1]);
        }
    }
}

$server->on('workerStart', function () use ($SQLITE_PATH) {
    Db::open($SQLITE_PATH);
    gc_disable(); // request handling creates no reference cycles
});

// Every statement's cursor is closed right after use: an open one keeps its read transaction
// (and a write's commit) open.
$server->on('request', function (Request $req, Response $res) use ($JWT_SECRET, $START) {
    $res->header('Content-Type', JSON_CT);
    $uri = $req->server['request_uri'];
    $method = $req->server['request_method'];
    try {
        if ($uri === '/feed' && $method === 'GET') {
            $s = Db::$feed;
            $s->execute();
            $body = '';
            while ($row = $s->fetch(PDO::FETCH_NUM)) $body .= ',' . post_json($row);
            $s->closeCursor();
            $res->end('{"posts":[' . substr($body, 1) . ']}');
            return;
        }
        if (str_starts_with($uri, '/posts/')) {
            if ($method === 'GET') {
                $id = parse_id(substr($uri, 7));
                if ($id <= 0) { $res->status(400); $res->end(E_BAD_ID); return; }
                $s = Db::$post;
                $s->bindValue(1, $id, PDO::PARAM_INT);
                $s->execute();
                $row = $s->fetch(PDO::FETCH_NUM);
                $s->closeCursor();
                if ($row === false) { $res->status(404); $res->end(E_POST_NOT_FOUND); return; }
                $res->end('{"post":' . post_json($row) . '}');
                return;
            }
            if ($method === 'POST' && str_ends_with($uri, '/like')) {
                $user = authenticate($req, $JWT_SECRET);
                if (is_string($user)) { $res->status(401); $res->end($user); return; }
                $id = parse_id(substr($uri, 7, -5));
                if ($id <= 0) { $res->status(400); $res->end(E_BAD_ID); return; }
                Writes::submit($res, 'like_write', $user[0], $id);
                return;
            }
        } elseif ($uri === '/posts' && $method === 'POST') {
            $user = authenticate($req, $JWT_SECRET);
            if (is_string($user)) { $res->status(401); $res->end($user); return; }
            $data = json_decode((string) $req->rawContent(), true);
            if ($data === null && json_last_error() !== JSON_ERROR_NONE) { $res->status(400); $res->end(E_MALFORMED); return; }
            $text = is_array($data) ? ($data['body'] ?? null) : null;
            if (!is_string($text) || ($text = trim($text, " \t\n\r\v\f")) === '') { $res->status(400); $res->end(E_BODY_REQUIRED); return; }
            if (strlen($text) > 500 && preg_match_all('/./su', $text) > 500) { $res->status(400); $res->end(E_BODY_TOO_LONG); return; }
            Writes::submit($res, 'post_write', $user, $text);
            return;
        } elseif ($uri === '/health' && $method === 'GET') {
            try {
                Db::$health->execute();
                Db::$health->fetchAll();
            } catch (Throwable $e) {
                $res->status(503);
                $res->end('{"status":"degraded","db":"unreachable","error":' . json_encode($e->getMessage(), JSON_INVALID_UTF8_SUBSTITUTE) . '}');
                return;
            }
            $res->end('{"status":"ok","db":"ok","uptime_s":' . (time() - $START) . '}');
            return;
        }
        $res->status(404);
        $res->end(E_NOT_FOUND);
    } catch (Throwable $e) {
        fwrite(STDERR, $e . "\n");
        foreach ([Db::$feed, Db::$post, Db::$insertPost, Db::$insertLike, Db::$postExists] as $stmt) $stmt->closeCursor();
        $res->status(500);
        $res->end(E_INTERNAL);
    }
});

$server->start();
