// Mirrors the snapshot schema from server/web_bridge.py exactly.

export type Phase = "SELECT" | "LOADING" | "PLAYING" | "RESULTS";
export type PickMode = "owner" | "assigned" | "rotate";

export interface ServerInfo {
  name: string;
  now: number;
  started_at: number;
  clients_online: number;
}

export interface ConsoleEntry {
  mid: string;
  name: string;
  mod: string;
  lobby: string | null;
}

export interface VoteInfo {
  kind: string;
  agreed: string[];
  need: number;
  ends_at: number;
}

export interface RoundSnap {
  id: number;
  pv: number;
  name: string;
  leader: string | null;
  players: string[];
  spectators: string[];
  dropped: string[];
  started: boolean;
  launch_at: number | null;
}

export interface MemberSnap {
  mid: string;
  name: string;
  connected: boolean;
  role: string;
  loaded: boolean;
  ready: boolean;
  reason: string;
  autoplay: boolean;
  back_in_sel: boolean;
  score: number;
  combo: number;
  pct: number;
  life: number;
  final: {
    score: number;
    pct: number;
    max_combo: number;
    life: number;
    jc: number[];
  } | null;
}

export interface StandingRow {
  mid: string;
  name: string;
  score: number;
  pct: number;
  max_combo: number;
  life: number;
  rank: number;
  final: boolean;
  connected: boolean;
  autoplay: boolean;
}

export interface Standings {
  complete: boolean;
  e: StandingRow[];
}

export interface LobbySnap {
  code: string;
  phase: Phase;
  pick_mode: PickMode;
  owner: string | null;
  leader: string | null;
  assigned: string | null;
  pending_pick_mode: { mode: PickMode; assigned: string | null } | null;
  group_paused: boolean;
  vote: VoteInfo | null;
  sel: { pv: number; name: string } | null;
  round: RoundSnap | null;
  members: MemberSnap[];
  standings: Standings | null;
}

export interface You {
  name: string | null;
  admin: boolean;
  owns: string[];
}

export interface Snapshot {
  t: "snapshot";
  server: ServerInfo;
  consoles: ConsoleEntry[];
  lobbies: LobbySnap[];
  you: You;
}

export interface FeedEntry {
  ts: number;
  kind: string;
  ev?: string;
  mid?: string | null;
  name?: string | null;
  value?: number | null;
  pv?: number | null;
  round?: number;
  vote?: string;
  agreed?: number;
  need?: number;
  expired?: boolean;
  op?: string;
  by?: string | null;
  mode?: string;
}

export interface HelloMsg {
  t: "hello";
  feed: Record<string, FeedEntry[]>;
}

export interface FeedMsg {
  t: "feed";
  lobby: string;
  entry: FeedEntry;
}

export interface ReplyMsg {
  t: "reply";
  id: number;
  ok: boolean;
  error?: string;
  you?: You;
}

export type ServerMsg = Snapshot | HelloMsg | FeedMsg | ReplyMsg;
