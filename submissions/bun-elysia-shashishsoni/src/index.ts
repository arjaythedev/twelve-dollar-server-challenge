import { Elysia } from "elysia";
import { verifyBearerToken } from "./auth";
import { loadConfig } from "./config";
import { startDescriptorWatch } from "./connections";
import {
  checkHealth,
  createLike,
  createPost,
  findPostById,
  listNewestPosts,
  openDatabase,
} from "./db";
import { ApiError, notFound, postNotFound } from "./errors";
import { errorResponse, jsonResponse } from "./http";
import { extractTrimmedBody, isMissingSentinel, parsePostId, readJsonBody } from "./validation";

const config = loadConfig();
openDatabase(config.sqlitePath);
startDescriptorWatch();

function uptimeSeconds(): number {
  return Math.floor((Date.now() - config.startedAtMs) / 1000);
}

const app = new Elysia({ aot: true, normalize: false })
  .get("/health", () => {
    try {
      checkHealth();
    } catch (err) {
      const message = err instanceof Error ? err.message : "unreachable";
      return jsonResponse(503, { status: "degraded", db: "unreachable", error: message });
    }
    return jsonResponse(200, { status: "ok", db: "ok", uptime_s: uptimeSeconds() });
  })
  .get("/feed", () => {
    try {
      return jsonResponse(200, { posts: listNewestPosts() });
    } catch (err) {
      return errorResponse(err);
    }
  })
  .get("/posts/:id", ({ params }) => {
    try {
      const id = parsePostId(params.id);
      if (isMissingSentinel(id)) throw postNotFound();
      const post = findPostById(id);
      if (!post) throw postNotFound();
      return jsonResponse(200, { post });
    } catch (err) {
      return errorResponse(err);
    }
  })
  .post("/posts", async ({ request }) => {
    try {
      const user = verifyBearerToken(request.headers.get("authorization"), config.jwtSecret);
      const payload = await readJsonBody(request);
      const body = extractTrimmedBody(payload);
      const created = createPost(user.userId, body);
      return jsonResponse(201, {
        post: {
          id: created.id,
          body,
          created_at: created.created_at,
          author: user.username,
          like_count: 0,
        },
      });
    } catch (err) {
      return errorResponse(err);
    }
  })
  .post("/posts/:id/like", ({ request, params }) => {
    try {
      const user = verifyBearerToken(request.headers.get("authorization"), config.jwtSecret);
      const id = parsePostId(params.id);
      if (isMissingSentinel(id)) throw postNotFound();
      const outcome = createLike(user.userId, id);
      if (outcome === "missing") throw postNotFound();
      if (outcome === "created") {
        return jsonResponse(201, { liked: true, already_liked: false, post_id: id });
      }
      return jsonResponse(200, { liked: true, already_liked: true, post_id: id });
    } catch (err) {
      return errorResponse(err);
    }
  })
  .all("*", () => jsonResponse(404, { error: "not found" }))
  .onError(({ error }) => {
    if (error instanceof ApiError) {
      return jsonResponse(error.status, { error: error.message });
    }
    if (error instanceof Error && "status" in error && (error as { status: number }).status === 404) {
      return jsonResponse(404, { error: "not found" });
    }
    return jsonResponse(500, { error: "internal server error" });
  });

app.listen(
  {
    hostname: config.host,
    port: config.port,
    idleTimeout: config.idleTimeoutSec,
  } as Parameters<Elysia["listen"]>[0],
  () => {
    console.error(`listening on ${config.host}:${config.port}`);
  },
);

export type App = typeof app;
export { notFound };
