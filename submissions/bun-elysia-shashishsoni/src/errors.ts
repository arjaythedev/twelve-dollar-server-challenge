export class ApiError extends Error {
  constructor(
    public readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

export function missingBearer(): ApiError {
  return new ApiError(401, "missing bearer token");
}

export function invalidToken(): ApiError {
  return new ApiError(401, "invalid or expired token");
}

export function invalidTokenPayload(): ApiError {
  return new ApiError(401, "invalid token payload");
}

export function invalidPostId(): ApiError {
  return new ApiError(400, "invalid post id");
}

export function postNotFound(): ApiError {
  return new ApiError(404, "post not found");
}

export function notFound(): ApiError {
  return new ApiError(404, "not found");
}

export function malformedJson(): ApiError {
  return new ApiError(400, "malformed JSON body");
}

export function bodyRequired(): ApiError {
  return new ApiError(400, "body is required");
}

export function bodyTooLong(): ApiError {
  return new ApiError(400, "body must be at most 500 characters");
}
