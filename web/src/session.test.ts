import { describe, expect, it } from "vitest";
import { clearStatus, hasSessionResults, sessionOrder } from "./session";
import type { MemberSnap } from "./types";

const row = (o: Partial<{ pct: number; border: number; clear: boolean | null; drop: boolean }>) => ({
  pct: 70,
  border: 0,
  clear: null,
  drop: false,
  ...o,
});

describe("clearStatus", () => {
  it("trusts the console's verdict over the percentage", () => {
    // passed the border mid-song, finished under it
    expect(clearStatus(row({ pct: 85, border: 90, clear: false }), true)).toBe("fail");
    // a song cleared below 80 %
    expect(clearStatus(row({ pct: 45, border: 40, clear: true }), true)).toBe("clear");
  });

  it("drop out or no final is DROP×OUT", () => {
    expect(clearStatus(row({ clear: false, drop: true }), true)).toBe("dnf");
    expect(clearStatus(row({}), false)).toBe("dnf");
  });

  it("falls back to the reported border, never a fixed 80 %", () => {
    expect(clearStatus(row({ pct: 61, border: 60 }), true)).toBe("clear");
    expect(clearStatus(row({ pct: 59, border: 60 }), true)).toBe("fail");
    expect(clearStatus(row({ pct: 95 }), true)).toBe("unknown");
  });
});

const mem = (name: string, pos: number, places: [number, number, number] = [0, 0, 0], score = 0) =>
  ({ name, mid: name, session: { pos, wins: places[0], score, places } }) as unknown as MemberSnap;

describe("session order", () => {
  it("sorts by the server's session position, join order on ties", () => {
    const ms = [mem("a", 3), mem("b", 1), mem("c", 2), mem("d", 0)];
    expect(sessionOrder(ms).map((m) => m.name)).toEqual(["b", "c", "a", "d"]);
  });

  it("knows when any round has counted", () => {
    expect(hasSessionResults([mem("a", 1), mem("b", 2)])).toBe(false);
    expect(hasSessionResults([mem("a", 1), mem("b", 2, [0, 0, 1], 0.5)])).toBe(true);
  });
});
