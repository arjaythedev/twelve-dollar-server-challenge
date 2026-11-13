import type { FastifyError, FastifyInstance } from "fastify";

import Fastify from "fastify";

import { authenticate, sendAuthError } from "./auth.js";
import {
  feedStmt,
  postStmt,
  insertPostStmt,
  insertLikeStmt,
  postExistsStmt,
  healthStmt,
} from "./db.js";
import { MAX_ID, parseId } from "./ids.js";
import { sendError, sendJson, sendRawJson } from "./responses.js";

export function buildApp(): FastifyInstance {
  const app = Fastify({
    logger: false,
    trustProxy: false,
    bodyLimit: 16384,
    keepAliveTimeout: 75000,
    requestTimeout: 0,
    connectionTimeout: 0,
    maxRequestsPerSocket: 0,
    exposeHeadRoutes: false,
  });
  app.server.headersTimeout = 80000;

  app.addContentTypeParser("application/json", { parseAs: "buffer" }, (request, body, done) => {
    done(null, body);
  });
  app.addContentTypeParser("*", { parseAs: "buffer" }, (request, body, done) => {
    done(null, body);
  });

  app.setNotFoundHandler((_request, reply) => {
    sendError(reply, 404, "not found");
  });

  app.setErrorHandler<FastifyError>((err, _request, reply) => {
    const status =
      typeof err.statusCode === "number" && err.statusCode >= 400 && err.statusCode < 600
        ? err.statusCode
        : 500;
    if (status === 404) {
      sendError(reply, 404, "not found");
      return;
    }
    if (status === 413) {
      sendError(reply, 413, "content too large");
      return;
    }
    sendError(reply, 500, "internal server error");
  });

  app.get("/health", (_request, reply) => {
    try {
      healthStmt.get();
    } catch (err) {
      sendJson(reply, 503, { status: "degraded", db: "unreachable", error: String(err) });
      return;
    }
    sendJson(reply, 200, { status: "ok", db: "ok", uptime_s: Math.floor(process.uptime()) });
  });

  app.get("/feed", (_request, reply) => {
    sendRawJson(reply, 200, feedStmt.get() ?? Buffer.from('{"posts":[]}'));
  });

  app.get<{ Params: { post_id: string } }>("/posts/:post_id", (request, reply) => {
    const pid = parseId(request.params.post_id);
    if (pid === null) {
      sendError(reply, 400, "invalid post id");
      return;
    }
    if (pid > MAX_ID) {
      sendError(reply, 404, "post not found");
      return;
    }
    const body = postStmt.get(pid);
    if (body === undefined) {
      sendError(reply, 404, "post not found");
      return;
    }
    sendRawJson(reply, 200, body);
  });

  app.post("/posts", (request, reply) => {
    const auth = authenticate(request);
    if (auth.kind !== "ok") {
      sendAuthError(reply, auth);
      return;
    }
    let data: unknown;
    try {
      const raw: unknown = request.body;
      const text = Buffer.isBuffer(raw) ? raw.toString("utf8") : typeof raw === "string" ? raw : "";
      if (text.length === 0) throw new Error("empty");
      data = JSON.parse(text);
    } catch {
      sendError(reply, 400, "malformed JSON body");
      return;
    }
    const body =
      typeof data === "object" && data !== null && !Array.isArray(data)
        ? (data as Record<string, unknown>)["body"]
        : undefined;
    if (typeof body !== "string") {
      sendError(reply, 400, "body is required");
      return;
    }
    const trimmed = body.trim();
    if (trimmed.length === 0) {
      sendError(reply, 400, "body is required");
      return;
    }
    if (trimmed.length > 500 && [...trimmed].length > 500) {
      sendError(reply, 400, "body must be at most 500 characters");
      return;
    }
    const created = insertPostStmt.get(auth.userId, trimmed, auth.username);
    if (created === undefined) {
      sendError(reply, 500, "internal server error");
      return;
    }
    sendRawJson(reply, 201, created);
  });

  app.post<{ Params: { post_id: string } }>("/posts/:post_id/like", (request, reply) => {
    const auth = authenticate(request);
    if (auth.kind !== "ok") {
      sendAuthError(reply, auth);
      return;
    }
    const pid = parseId(request.params.post_id);
    if (pid === null) {
      sendError(reply, 400, "invalid post id");
      return;
    }
    if (pid <= MAX_ID) {
      const info = insertLikeStmt.run(auth.userId, pid, pid);
      if (info.changes === 1) {
        sendRawJson(
          reply,
          201,
          Buffer.from(`{"liked":true,"already_liked":false,"post_id":${pid}}`),
        );
        return;
      }
      if (postExistsStmt.get(pid) !== undefined) {
        sendRawJson(
          reply,
          200,
          Buffer.from(`{"liked":true,"already_liked":true,"post_id":${pid}}`),
        );
        return;
      }
    }
    sendError(reply, 404, "post not found");
  });

  return app;
}
