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
});
