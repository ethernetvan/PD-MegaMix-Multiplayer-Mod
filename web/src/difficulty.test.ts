import { describe, expect, it } from "vitest";
import {
  diffClass,
  diffLabel,
  diffShort,
  mixedDifficulty,
  platLabel,
} from "./difficulty";

describe("difficulty labels", () => {
  it("names the four difficulties and the EXTRA edition", () => {
    expect([0, 1, 2, 3].map((d) => diffLabel(d, 0))).toEqual([
      "EASY",
      "NORMAL",
      "HARD",
      "EXTREME",
    ]);
    expect(diffLabel(3, 1)).toBe("EXTRA EXTREME");
    expect(diffShort(3, 1)).toBe("EXEX");
    expect(diffShort(2)).toBe("H");
    expect(diffClass(3, 1)).toBe("exextreme");
    expect(diffClass(0)).toBe("easy");
  });

  it("the EXTRA edition only exists on EXTREME", () => {
    expect(diffLabel(1, 1)).toBe("NORMAL");
    expect(diffClass(2, 1)).toBe("hard");
  });

  it("is null for consoles that report none", () => {
    for (const d of [-1, 4, 1.5, undefined, null]) {
      expect(diffLabel(d)).toBeNull();
      expect(diffShort(d)).toBeNull();
      expect(diffClass(d)).toBeNull();
    }
  });
});

describe("mixedDifficulty", () => {
  it("is true only when known charts differ", () => {
    expect(mixedDifficulty([{ diff: 2 }, { diff: 2 }])).toBe(false);
    expect(mixedDifficulty([{ diff: 2 }, { diff: 3 }])).toBe(true);
    expect(mixedDifficulty([{ diff: 3, ed: 0 }, { diff: 3, ed: 1 }])).toBe(true);
    // an old client next to a known one is not a mix
    expect(mixedDifficulty([{ diff: 2 }, { diff: -1 }, {}])).toBe(false);
    expect(mixedDifficulty([])).toBe(false);
  });
});

describe("platLabel", () => {
  it("labels the two versions of the mod, nothing for bots", () => {
    expect(platLabel("pc")).toBe("PC · Mega Mix+");
    expect(platLabel("switch")).toBe("Switch · Mega Mix");
    expect(platLabel("bot")).toBeNull();
    expect(platLabel(undefined)).toBeNull();
  });
});
