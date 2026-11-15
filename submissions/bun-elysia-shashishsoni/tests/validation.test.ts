import { describe, expect, test } from "bun:test";
import { extractTrimmedBody, parsePostId } from "../src/validation";
import { ApiError } from "../src/errors";

describe("parsePostId", () => {
  test("accepts plain positive integers", () => {
    expect(parsePostId("1")).toBe(1);
    expect(parsePostId("500000")).toBe(500000);
  });

  test("rejects zero, negative, decimal, alpha", () => {
    for (const bad of ["0", "-1", "abc", "1.5", "", " 1", "1 "]) {
      expect(() => parsePostId(bad)).toThrow(ApiError);
    }
  });
});

describe("extractTrimmedBody", () => {
  test("trims and accepts normal bodies", () => {
    expect(extractTrimmedBody({ body: "  hi  " })).toBe("hi");
  });

  test("rejects missing, non-string, blank", () => {
    for (const payload of [{}, { body: "" }, { body: "   " }, { body: 123 }]) {
      expect(() => extractTrimmedBody(payload)).toThrow(ApiError);
    }
  });

  test("rejects bodies over 500 chars after trim", () => {
    expect(() => extractTrimmedBody({ body: `  ${"a".repeat(501)} ` })).toThrow(ApiError);
    expect(extractTrimmedBody({ body: `  ${"b".repeat(500)} ` })).toBe("b".repeat(500));
  });
});
