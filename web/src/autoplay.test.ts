import { describe, it, expect } from "vitest";
import { autoLabel } from "./autoplay";

describe("autoLabel", () => {
  it("shows plain AUTO at 100 and undefined", () => {
    expect(autoLabel(100)).toBe("AUTO");
    expect(autoLabel(undefined)).toBe("AUTO");
    expect(autoLabel(null)).toBe("AUTO");
  });
  it("shows the percentage below 100", () => {
    expect(autoLabel(50)).toBe("AUTO 50%");
    expect(autoLabel(0)).toBe("AUTO 0%");
    expect(autoLabel(75)).toBe("AUTO 75%");
  });
  it("shows the bot mode tag when a mode is set", () => {
    expect(autoLabel(97, "pro")).toBe("AUTO PRO");
    expect(autoLabel(97, "97")).toBe("AUTO 97");
    expect(autoLabel(94, "streak")).toBe("AUTO STREAK");
    expect(autoLabel(95, "human")).toBe("AUTO HUMAN");
    expect(autoLabel(95, "clutch")).toBe("AUTO CLUTCH");
    expect(autoLabel(95, "tie")).toBe("AUTO TIE");
    expect(autoLabel(100, "auto")).toBe("AUTO");
  });
  it("ignores unknown and custom modes and falls back to pct", () => {
    expect(autoLabel(50, "custom")).toBe("AUTO 50%");
    expect(autoLabel(80, "bogus")).toBe("AUTO 80%");
  });
});
