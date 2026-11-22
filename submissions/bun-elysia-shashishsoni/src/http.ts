import { shedProbabilityNow } from "./connections";
import { ApiError } from "./errors";

const JSON_HEADERS = { "content-type": "application/json" } as const;
const JSON_CLOSE_HEADERS = { "content-type": "application/json", connection: "close" } as const;

export function jsonResponse(status: number, payload: unknown): Response {
  return new Response(JSON.stringify(payload), {
    status,
    headers: Math.random() < shedProbabilityNow() ? JSON_CLOSE_HEADERS : JSON_HEADERS,
  });
}

export function errorResponse(err: unknown): Response {
  if (err instanceof ApiError) {
    return jsonResponse(err.status, { error: err.message });
  }
  return jsonResponse(500, { error: "internal server error" });
}
