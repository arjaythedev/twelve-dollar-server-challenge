import { describe, expect, test } from "bun:test";
import { parseOpenFilesLimit, shouldRetire } from "../src/connections";

describe("shouldRetire", () => {
  test("starts retiring above 92% of the limit", () => {
    expect(shouldRetire(60_500, 65_535, false)).toBe(true);
  });

  test("stops retiring below 88% of the limit", () => {
    expect(shouldRetire(57_000, 65_535, true)).toBe(false);
  });

  test("keeps its current state in between", () => {
    expect(shouldRetire(59_000, 65_535, true)).toBe(true);
    expect(shouldRetire(59_000, 65_535, false)).toBe(false);
  });
});

describe("parseOpenFilesLimit", () => {
  test("reads the soft limit from /proc/self/limits", () => {
    const limits = "Max cpu time              unlimited            unlimited            seconds\n" +
      "Max open files            65535                65535                files\n";
    expect(parseOpenFilesLimit(limits)).toBe(65_535);
  });

  test("returns 0 when the line is missing", () => {
    expect(parseOpenFilesLimit("")).toBe(0);
  });
});
