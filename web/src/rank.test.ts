import { describe, expect, it } from "vitest";
import { competitionRanks } from "./rank";

describe("competitionRanks", () => {
  it("gives tied rows the lowest position of their group (1-3-3-4)", () => {
    const rows = [
      { pct: 95, score: 100 },
      { pct: 90, score: 100 },
      { pct: 90, score: 100 },
      { pct: 40, score: 40 },
    ];
    expect(competitionRanks(rows)).toEqual([1, 3, 3, 4]);
  });

  it("two tied for first are both 2nd", () => {
    const rows = [
      { pct: 90, score: 100 },
      { pct: 90, score: 100 },
      { pct: 50, score: 50 },
    ];
    expect(competitionRanks(rows)).toEqual([2, 2, 3]);
  });

  it("breaks ties on score", () => {
    const rows = [
      { pct: 90, score: 200 },
      { pct: 90, score: 100 },
    ];
    expect(competitionRanks(rows)).toEqual([1, 2]);
  });

  it("rounds pct to 2 decimals for the key", () => {
    const rows = [
      { pct: 90.004, score: 1 },
      { pct: 90.001, score: 1 },
      { pct: 89.994, score: 1 },
    ];
    expect(competitionRanks(rows)).toEqual([2, 2, 3]);
  });

  it("handles empty and single rows", () => {
    expect(competitionRanks([])).toEqual([]);
    expect(competitionRanks([{ pct: 0, score: 0 }])).toEqual([1]);
  });
});
