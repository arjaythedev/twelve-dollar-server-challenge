<?php

declare(strict_types=1);

namespace App\Controllers;

use CodeIgniter\HTTP\ResponseInterface;
use JsonException;
use PDO;
use PDOStatement;
use Throwable;

final class Feed extends BaseController
{
    private const SELECT_POST = 'SELECT p.id, p.body, p.created_at, u.username AS author,
        (SELECT count(*) FROM likes l WHERE l.post_id = p.id) AS like_count
        FROM posts p JOIN users u ON u.id = p.user_id';

    private static ?PDO $connection = null;

    /** Query plans only; every execution reads current database contents. */
    private static array $statements = [];

    private function db(): PDO
    {
        if (self::$connection === null) {
            $path = getenv('SQLITE_PATH');
            if (!$path || !is_file($path)) {
                throw new \RuntimeException('SQLITE_PATH must name an existing database');
            }
            self::$connection = new PDO('sqlite:' . $path, null, null, [
                PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
                PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
                PDO::ATTR_TIMEOUT => 5,
            ]);
            // WAL is established at startup; these settings belong to each connection.
            self::$connection->exec('PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;
                PRAGMA mmap_size=268435456; PRAGMA cache_size=-8192; PRAGMA temp_store=MEMORY');
        }
        return self::$connection;
    }

    private function statement(string $sql, array $parameters = []): PDOStatement
    {
        $statement = self::$statements[$sql] ??= $this->db()->prepare($sql);
        $statement->execute($parameters);
        return $statement;
    }

    private function json(array $data, int $status = 200): ResponseInterface
    {
        return $this->response->setStatusCode($status)->setJSON(json_encode(
            $data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR
        ));
    }

    private function error(string $message, int $status): ResponseInterface
    {
        return $this->json(['error' => $message], $status);
    }

    private function guarded(callable $action): ResponseInterface
    {
        try {
            return $action();
        } catch (Throwable $e) {
            foreach (self::$statements as $statement) {
                $statement->closeCursor();
            }
            log_message('error', '{exception}', ['exception' => $e]);
            return $this->error('internal server error', 500);
        }
    }

    private function id(string $raw): ?int
    {
        return preg_match('/^[1-9][0-9]*$/D', $raw) && strlen($raw) <= 16
            && (float) $raw <= 9007199254740992 ? (int) $raw : null;
    }

    /** Verify every signature; neither identities nor verification results are cached. */
    private function authenticate(): array|ResponseInterface
    {
        $authorization = $this->request->getHeaderLine('Authorization');
        if (!str_starts_with($authorization, 'Bearer ')) {
            return $this->error('missing bearer token', 401);
        }
        try {
            $parts = explode('.', substr($authorization, 7));
            if (count($parts) !== 3) {
                throw new \UnexpectedValueException();
            }
            [$header, $payload, $signature] = $parts;
            $decode = static function (string $part): string {
                if (!preg_match('/^[A-Za-z0-9_-]+$/D', $part)) {
                    throw new \UnexpectedValueException();
                }
                $decoded = base64_decode(strtr($part, '-_', '+/'), true);
                if ($decoded === false) {
                    throw new \UnexpectedValueException();
                }
                return $decoded;
            };
            $h = json_decode($decode($header), true, 32, JSON_THROW_ON_ERROR);
            $p = json_decode($decode($payload), true, 32, JSON_THROW_ON_ERROR);
            $secret = getenv('JWT_SECRET');
            if (!$secret || !is_array($h) || ($h['alg'] ?? null) !== 'HS256'
                || !hash_equals(hash_hmac('sha256', "$header.$payload", $secret, true), $decode($signature))
                || !is_array($p) || !isset($p['exp'])
                || !(is_int($p['exp']) || is_float($p['exp'])) || $p['exp'] <= time()
                || (isset($p['nbf']) && (!(is_int($p['nbf']) || is_float($p['nbf'])) || $p['nbf'] > time()))) {
                throw new \UnexpectedValueException();
            }
        } catch (Throwable) {
            return $this->error('invalid or expired token', 401);
        }
        if (!is_string($p['sub'] ?? null) || $this->id($p['sub']) === null || !is_string($p['username'] ?? null)) {
            return $this->error('invalid token payload', 401);
        }
        return [(int) $p['sub'], $p['username']];
    }

    public function notFound(): ResponseInterface
    {
        return $this->error('not found', 404);
    }

    public function health(): ResponseInterface
    {
        try {
            $statement = $this->statement('SELECT 1');
            $statement->fetchColumn();
            $statement->closeCursor();
            return $this->json(['status' => 'ok', 'db' => 'ok', 'uptime_s' => max(0, time() - (int) getenv('APP_STARTED_AT'))]);
        } catch (Throwable $e) {
            return $this->json(['status' => 'degraded', 'db' => 'unreachable', 'error' => $e->getMessage()], 503);
        }
    }

    public function index(): ResponseInterface
    {
        return $this->guarded(function () {
            $statement = $this->statement(self::SELECT_POST . ' ORDER BY p.created_at DESC, p.id DESC LIMIT 20');
            $posts = $statement->fetchAll();
            $statement->closeCursor();
            return $this->json(['posts' => $posts]);
        });
    }

    public function show(string $raw): ResponseInterface
    {
        if (($id = $this->id($raw)) === null) {
            return $this->error('invalid post id', 400);
        }
        return $this->guarded(function () use ($id) {
            $statement = $this->statement(self::SELECT_POST . ' WHERE p.id = ?', [$id]);
            $post = $statement->fetch();
            $statement->closeCursor();
            return $post ? $this->json(['post' => $post]) : $this->error('post not found', 404);
        });
    }

    public function create(): ResponseInterface
    {
        $user = $this->authenticate();
        if ($user instanceof ResponseInterface) {
            return $user;
        }
        try {
            $data = json_decode($this->request->getBody(), true, 32, JSON_THROW_ON_ERROR);
        } catch (JsonException) {
            return $this->error('malformed JSON body', 400);
        }
        $body = is_array($data) ? ($data['body'] ?? null) : null;
        if (!is_string($body) || ($body = trim($body, " \t\n\r\v\f")) === '') {
            return $this->error('body is required', 400);
        }
        if (mb_strlen($body, 'UTF-8') > 500) {
            return $this->error('body must be at most 500 characters', 400);
        }
        return $this->guarded(function () use ($user, $body) {
            $statement = $this->statement('INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at', [$user[0], $body]);
            $row = $statement->fetch();
            // Finalize RETURNING before responding: SQLite commits the autocommit write here.
            $statement->closeCursor();
            return $this->json(['post' => ['id' => $row['id'], 'body' => $body,
                'created_at' => $row['created_at'], 'author' => $user[1], 'like_count' => 0]], 201);
        });
    }

    public function like(string $raw): ResponseInterface
    {
        $user = $this->authenticate();
        if ($user instanceof ResponseInterface) {
            return $user;
        }
        if (($id = $this->id($raw)) === null) {
            return $this->error('invalid post id', 400);
        }
        return $this->guarded(function () use ($user, $id) {
            $statement = $this->statement('INSERT INTO likes (user_id, post_id)
                SELECT ?, ? WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?)
                ON CONFLICT (user_id, post_id) DO NOTHING', [$user[0], $id, $id]);
            $inserted = $statement->rowCount() === 1;
            $statement->closeCursor();
            if (!$inserted) {
                $exists = $this->statement('SELECT 1 FROM posts WHERE id = ?', [$id]);
                $found = $exists->fetchColumn();
                $exists->closeCursor();
                if (!$found) {
                    return $this->error('post not found', 404);
                }
            }
            return $this->json(['liked' => true, 'already_liked' => !$inserted, 'post_id' => $id], $inserted ? 201 : 200);
        });
    }
}
