import { describe, expect, it } from "vitest";
import { fmtElapsed, fmtPct, fmtRel, fmtScore } from "./format";

describe("fmtScore", () => {
  it("groups thousands", () => {
    expect(fmtScore(1234567)).toBe("1,234,567");
    expect(fmtScore(0)).toBe("0");
    expect(fmtScore(950)).toBe("950");
  });
});

describe("fmtPct", () => {
  it("two decimals", () => {
    expect(fmtPct(95.5)).toBe("95.50%");
    expect(fmtPct(100.126)).toBe("100.13%");
    expect(fmtPct(0)).toBe("0.00%");
  });
});

describe("fmtRel", () => {
  const now = 1_000_000;
  it("relative times", () => {
    expect(fmtRel(now - 1, now)).toBe("just now");
    expect(fmtRel(now - 30, now)).toBe("30s ago");
    expect(fmtRel(now - 300, now)).toBe("5m ago");
    expect(fmtRel(now - 7200, now)).toBe("2h ago");
    expect(fmtRel(now - 200_000, now)).toBe("2d ago");
  });
  it("clamps future timestamps", () => {
    expect(fmtRel(now + 60, now)).toBe("just now");
  });
});

describe("fmtElapsed", () => {
  it("m:ss and h:mm:ss", () => {
    expect(fmtElapsed(900, 1000)).toBe("1:40");
    expect(fmtElapsed(0, 65)).toBe("1:05");
    expect(fmtElapsed(0, 3661)).toBe("1:01:01");
  });
  it("null start and future start", () => {
    expect(fmtElapsed(null, 100)).toBe("—");
    expect(fmtElapsed(200, 100)).toBe("0:00");
  });
});
