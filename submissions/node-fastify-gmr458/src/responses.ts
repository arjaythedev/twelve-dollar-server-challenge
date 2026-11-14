import type { FastifyReply } from "fastify";

export function sendJson(reply: FastifyReply, status: number, body: unknown): void {
  reply.code(status).header("content-type", "application/json").send(JSON.stringify(body));
}

export function sendRawJson(reply: FastifyReply, status: number, raw: Buffer): void {
  reply.code(status).header("content-type", "application/json").send(raw);
}

export function sendError(reply: FastifyReply, status: number, message: string): void {
  sendJson(reply, status, { error: message });
}
