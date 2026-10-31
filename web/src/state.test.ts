import { describe, expect, it } from "vitest";
import { canAdmin, initialState, reducer, type AppState } from "./state";
import type { FeedEntry, LobbySnap, Snapshot, You } from "./types";

const YOU: You = { name: null, admin: false, owns: [] };

const lobby = (code: string): LobbySnap => ({
  code,
  phase: "SELECT",
  pick_mode: "rotate",
  owner: null,
  leader: null,
  assigned: null,
  pending_pick_mode: null,
  group_paused: false,
  vote: null,
  sel: null,
  round: null,
  members: [],
  standings: null,
});

const snap = (lobbies: LobbySnap[], you: You = YOU): Snapshot => ({
  t: "snapshot",
  server: { name: "srv", now: Date.now() / 1000, started_at: 0, clients_online: 0 },
  consoles: [],
  lobbies,
  you,
});

const ev = (n: number): FeedEntry => ({ ts: n, kind: "event", ev: "joined", name: "P" });

describe("reducer", () => {
  it("snapshot replaces snapshot, you, and computes clock offset", () => {
    const s0 = initialState;
    const s1 = reducer(s0, { type: "snapshot", snap: snap([lobby("ABCD")]) });
    expect(s1.snapshot?.lobbies[0].code).toBe("ABCD");
    expect(s1.you).toEqual(YOU);
    expect(s1.clockOffset).toBeCloseTo(0, 0);
  });

  it("hello merges the feed backlog", () => {
    const s = reducer(initialState, {
      type: "hello",
      feed: { ABCD: [ev(1), ev(2)] },
    });
    expect(s.feeds.ABCD).toHaveLength(2);
  });

  it("feed appends and caps at 100 per lobby", () => {
    let s: AppState = initialState;
    for (let i = 0; i < 105; i++)
      s = reducer(s, { type: "feed", lobby: "ABCD", entry: ev(i) });
    expect(s.feeds.ABCD).toHaveLength(100);
    expect(s.feeds.ABCD[0].ts).toBe(5);
    expect(s.feeds.ABCD[99].ts).toBe(104);
  });

  it("snapshot prunes feeds of lobbies that vanished", () => {
    let s = reducer(initialState, {
      type: "hello",
      feed: { ABCD: [ev(1)], WXYZ: [ev(2)] },
    });
    s = reducer(s, { type: "snapshot", snap: snap([lobby("ABCD")]) });
    expect(s.feeds.ABCD).toHaveLength(1);
    expect(s.feeds.WXYZ).toBeUndefined();
  });

  it("you and status updates", () => {
    let s = reducer(initialState, { type: "status", status: "live" });
    expect(s.status).toBe("live");
    const you: You = { name: "Alice", admin: false, owns: ["ABCD"] };
    s = reducer(s, { type: "you", you });
    expect(s.you.name).toBe("Alice");
    expect(s.you.owns).toEqual(["ABCD"]);
  });
});

describe("canAdmin", () => {
  it("admin sees all, owners see their lobby, others nothing", () => {
    expect(canAdmin({ name: null, admin: true, owns: [] }, "ABCD")).toBe(true);
    expect(canAdmin({ name: "A", admin: false, owns: ["ABCD"] }, "ABCD")).toBe(true);
    expect(canAdmin({ name: "A", admin: false, owns: ["ABCD"] }, "WXYZ")).toBe(false);
    expect(canAdmin(YOU, "ABCD")).toBe(false);
  });
});
