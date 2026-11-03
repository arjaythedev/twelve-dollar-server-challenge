<?php
// The $12 server challenge API on Swoole's HTTP server: one worker process, one SQLite connection,
// handlers run synchronously (no coroutines). See README.md for why.
declare(strict_types=1);

use Swoole\Http\Request;
use Swoole\Http\Response;
use Swoole\Http\Server;

function requireEnv(string $name): string
{
    $value = getenv($name);
    if ($value === false || $value === '') {
        fwrite(STDERR, "$name is required\n");
        exit(1);
    }
    return $value;
}

$dbPath = requireEnv('SQLITE_PATH');
$secret = requireEnv('JWT_SECRET');
$host = getenv('HOST') ?: '127.0.0.1';
$port = (int) (getenv('PORT') ?: 3000);

// Throwing makes an encoding failure (e.g. a non-UTF-8 row) a 500 instead of a 200 with broken JSON.
const JSON_FLAGS = JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR;
// The whitespace JavaScript's String.prototype.trim() strips, which the reference implementations use.
const TRIM_RE = '/^[\t\n\x0B\f\r \x{00A0}\x{1680}\x{2000}-\x{200A}\x{2028}\x{2029}\x{202F}\x{205F}\x{3000}\x{FEFF}]+'
    . '|[\t\n\x0B\f\r \x{00A0}\x{1680}\x{2000}-\x{200A}\x{2028}\x{2029}\x{202F}\x{205F}\x{3000}\x{FEFF}]+$/u';
const POST_SELECT = 'SELECT p.id, p.body, p.created_at, u.username AS author,
        (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
   FROM posts p JOIN users u ON u.id = p.user_id';

final class HttpError extends Exception
{
    public function __construct(public readonly int $status, string $message)
    {
        parent::__construct($message);
    }
}

// Opened inside the worker (onWorkerStart), never shared across a fork.
$db = null;
$dbError = '';
$stmt = [];
$started = time();

function openDb(string $path): PDO
{
    $db = new PDO('sqlite:' . $path, null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
    $db->exec('PRAGMA journal_mode = WAL');
    $db->exec('PRAGMA synchronous = NORMAL');
    $db->exec('PRAGMA busy_timeout = 5000');
    $db->exec('PRAGMA cache_size = -65536');      // 64 MiB page cache
    $db->exec('PRAGMA mmap_size = 268435456');    // 256 MiB
    $db->exec('PRAGMA temp_store = MEMORY');
    return $db;
}

/** Strict, unpadded base64url: anything outside the URL-safe alphabet is malformed. */
function b64url(string $s): string|false
{
    if (!preg_match('/^[A-Za-z0-9_-]*$/', $s)) {
        return false;
    }
    return base64_decode(strtr($s, '-_', '+/'), true);
}

/** Verifies the bearer token on every call (no caching) and returns [user id, username]. */
function authenticate(Request $req, string $secret): array
{
    $auth = $req->header['authorization'] ?? '';
    if (!str_starts_with($auth, 'Bearer ')) {
        throw new HttpError(401, 'missing bearer token');
    }
    $parts = explode('.', substr($auth, 7));
    if (count($parts) !== 3) {
        throw new HttpError(401, 'invalid or expired token');
    }
    [$h, $p, $s] = $parts;
    $header = json_decode((string) b64url($h), true);
    $sig = b64url($s);
    if (!is_array($header) || ($header['alg'] ?? null) !== 'HS256' || $sig === false
        || !hash_equals(hash_hmac('sha256', "$h.$p", $secret, true), $sig)) {
        throw new HttpError(401, 'invalid or expired token');
    }
    $payload = json_decode((string) b64url($p), true);
    if (!is_array($payload)) {
        throw new HttpError(401, 'invalid or expired token');
    }
    $exp = $payload['exp'] ?? null;
    if ((!is_int($exp) && !is_float($exp)) || $exp <= time()) {
        throw new HttpError(401, 'invalid or expired token');
    }
    $sub = $payload['sub'] ?? null;
    $username = $payload['username'] ?? null;
    if (!is_string($sub) || !preg_match('/^[1-9][0-9]*$/', $sub) || !is_string($username)) {
        throw new HttpError(401, 'invalid token payload');
    }
    return [(int) $sub, $username];
}

function postId(string $raw): int
{
    if (!ctype_digit($raw) || ($id = (int) $raw) <= 0) {
        throw new HttpError(400, 'invalid post id');
    }
    return $id;
}

function handle(Request $req, Response $res): void
{
    global $db, $dbError, $stmt, $secret, $started;

    $method = $req->server['request_method'];
    $path = $req->server['request_uri'];

    if ($method === 'GET' && $path === '/feed') {
        $stmt['feed']->execute();
        send($res, 200, '{"posts":' . json_encode($stmt['feed']->fetchAll(PDO::FETCH_ASSOC), JSON_FLAGS) . '}');
        return;
    }

    if (str_starts_with($path, '/posts/')) {
        $rest = substr($path, 7);
        if ($method === 'GET' && !str_contains($rest, '/')) {
            $s = $stmt['post'];
            $s->execute([postId($rest)]);
            $post = $s->fetch(PDO::FETCH_ASSOC);
            $s->closeCursor();
            if ($post === false) {
                throw new HttpError(404, 'post not found');
            }
            send($res, 200, '{"post":' . json_encode($post, JSON_FLAGS) . '}');
            return;
        }
        if ($method === 'POST' && str_ends_with($rest, '/like') && substr_count($rest, '/') === 1) {
            [$userId] = authenticate($req, $secret);
            $id = postId(substr($rest, 0, -5));
            $stmt['like']->execute([$userId, $id, $id]);
            if ($stmt['like']->rowCount() === 1) {
                send($res, 201, '{"liked":true,"already_liked":false,"post_id":' . $id . '}');
                return;
            }
            $stmt['exists']->execute([$id]);
            $exists = $stmt['exists']->fetchColumn() !== false;
            $stmt['exists']->closeCursor();
            if (!$exists) {
                throw new HttpError(404, 'post not found');
            }
            send($res, 200, '{"liked":true,"already_liked":true,"post_id":' . $id . '}');
            return;
        }
    }

    if ($method === 'POST' && $path === '/posts') {
        [$userId, $username] = authenticate($req, $secret);
        try {
            $data = json_decode((string) $req->rawContent(), true, 512, JSON_THROW_ON_ERROR);
        } catch (JsonException) {
            throw new HttpError(400, 'malformed JSON body');
        }
        $body = is_array($data) && is_string($data['body'] ?? null) ? preg_replace(TRIM_RE, '', $data['body']) : '';
        if ($body === '') {
            throw new HttpError(400, 'body is required');
        }
        if (mb_strlen($body, 'UTF-8') > 500) {
            throw new HttpError(400, 'body must be at most 500 characters');
        }
        // fetchAll steps the statement to completion, so the autocommit has happened before we reply.
        $stmt['create']->execute([$userId, $body]);
        [$row] = $stmt['create']->fetchAll(PDO::FETCH_ASSOC);
        send($res, 201, json_encode(['post' => [
            'id' => $row['id'],
            'body' => $body,
            'created_at' => $row['created_at'],
            'author' => $username,
            'like_count' => 0,
        ]], JSON_FLAGS));
        return;
    }

    if ($method === 'GET' && $path === '/health') {
        try {
            if ($db === null) {
                throw new RuntimeException($dbError);
            }
            $db->query('SELECT 1')->fetchColumn();
        } catch (Throwable $e) {
            send($res, 503, json_encode(['status' => 'degraded', 'db' => 'unreachable', 'error' => $e->getMessage()], JSON_FLAGS));
            return;
        }
        send($res, 200, '{"status":"ok","db":"ok","uptime_s":' . (time() - $started) . '}');
        return;
    }

    throw new HttpError(404, 'not found');
}

function send(Response $res, int $status, string $json): void
{
    $res->status($status);
    $res->header('Content-Type', 'application/json');
    $res->end($json);
}

$server = new Server($host, $port, SWOOLE_BASE);
$server->set([
    'worker_num' => 1,              // one vCPU: one process, no IPC or context switches
    'enable_coroutine' => false,    // handlers never yield; SQLite calls are sub-millisecond
    'max_connection' => 65535,      // capped by the open-file limit
    'backlog' => 4096,
    'http_parse_post' => false,
    'http_parse_cookie' => false,
    'http_parse_files' => false,
    'http_compression' => false,
    'log_level' => SWOOLE_LOG_WARNING,
]);

$server->on('workerStart', function () use ($dbPath) {
    global $db, $dbError, $stmt;
    // A failure here keeps the server up so /health can report 503; other routes return 500.
    try {
        $db = openDb($dbPath);
        $stmt = [
            'feed' => $db->prepare(POST_SELECT . ' ORDER BY p.created_at DESC, p.id DESC LIMIT 20'),
            'post' => $db->prepare(POST_SELECT . ' WHERE p.id = ?'),
            'create' => $db->prepare('INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at'),
            'like' => $db->prepare('INSERT INTO likes (user_id, post_id) SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?)
                                    ON CONFLICT (user_id, post_id) DO NOTHING'),
            'exists' => $db->prepare('SELECT 1 FROM posts WHERE id = ?'),
        ];
    } catch (Throwable $e) {
        error_log((string) $e);
        $db = null;
        $dbError = $e->getMessage();
    }
});

$server->on('request', function (Request $req, Response $res) {
    try {
        handle($req, $res);
    } catch (HttpError $e) {
        send($res, $e->status, json_encode(['error' => $e->getMessage()], JSON_FLAGS));
    } catch (Throwable $e) {
        error_log((string) $e);
        send($res, 500, '{"error":"internal server error"}');
    }
});

echo "listening on $host:$port\n";
$server->start();
