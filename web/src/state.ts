import type { FeedEntry, Snapshot, You } from "./types";

export type ConnStatus = "connecting" | "live" | "reconnecting";

export interface AppState {
  status: ConnStatus;
  snapshot: Snapshot | null;
  you: You;
  feeds: Record<string, FeedEntry[]>;
  clockOffset: number;
}

export const initialState: AppState = {
  status: "connecting",
  snapshot: null,
  you: { name: null, admin: false, owns: [] },
  feeds: {},
  clockOffset: 0,
};

export type Action =
  | { type: "status"; status: ConnStatus }
  | { type: "hello"; feed: Record<string, FeedEntry[]> }
  | { type: "snapshot"; snap: Snapshot }
  | { type: "feed"; lobby: string; entry: FeedEntry }
  | { type: "you"; you: You };

export const FEED_MAX = 100;

export function reducer(state: AppState, a: Action): AppState {
  switch (a.type) {
    case "status":
      return { ...state, status: a.status };
    case "hello": {
      const feeds: Record<string, FeedEntry[]> = {};
      for (const [code, list] of Object.entries(a.feed))
        feeds[code] = list.slice(-FEED_MAX);
      return { ...state, feeds };
    }
    case "snapshot": {
      const codes = new Set(a.snap.lobbies.map((l) => l.code));
      const feeds: Record<string, FeedEntry[]> = {};
      for (const [code, list] of Object.entries(state.feeds))
        if (codes.has(code)) feeds[code] = list;
      return {
        ...state,
        snapshot: a.snap,
        you: a.snap.you,
        feeds,
        clockOffset: a.snap.server.now - Date.now() / 1000,
      };
    }
    case "feed": {
      const list = [...(state.feeds[a.lobby] ?? []), a.entry].slice(-FEED_MAX);
      return { ...state, feeds: { ...state.feeds, [a.lobby]: list } };
    }
    case "you":
      return { ...state, you: a.you };
  }
}

export const canAdmin = (you: You, code: string): boolean =>
  you.admin || you.owns.includes(code);

// All server timestamps are absolute epochs; this is the current server
// time estimated from the last snapshot's clock offset.
export const serverNow = (s: AppState): number =>
  Date.now() / 1000 + s.clockOffset;
