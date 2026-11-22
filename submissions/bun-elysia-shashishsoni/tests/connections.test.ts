import { describe, expect, test } from "bun:test";
import { parseOpenFilesLimit, shedForLoad } from "../src/connections";

describe("shedForLoad", () => {
  test("sheds nothing at or below 85% of the limit", () => {
    expect(shedForLoad(55_000, 65_535)).toBe(0);
    expect(shedForLoad(55_704, 65_535)).toBe(0);
  });

  test("sheds everything at or above 95% of the limit", () => {
    expect(shedForLoad(62_259, 65_535)).toBe(1);
    expect(shedForLoad(65_535, 65_535)).toBe(1);
  });

  test("ramps linearly in between (90% sheds half)", () => {
    expect(shedForLoad(58_982, 65_535)).toBeCloseTo(0.5, 2);
  });

  test("returns 0 without a limit", () => {
    expect(shedForLoad(60_000, 0)).toBe(0);
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
