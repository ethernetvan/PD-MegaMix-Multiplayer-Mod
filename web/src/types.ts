// Mirrors the snapshot schema from server/web_bridge.py exactly.

export type Phase = "SELECT" | "LOADING" | "PLAYING" | "RESULTS";
export type PickMode = "owner" | "assigned" | "rotate";
export type Platform = "switch" | "pc" | "bot";

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
  /* which version of the mod: Switch (Mega Mix) or PC (Mega Mix+) */
  plat?: Platform;
  lobby: string | null;
  avatar: string | null;
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
  plat?: Platform;
  /* the member's chart: 0..3 = EASY NORMAL HARD EXTREME (-1 = not
     reported), ed 1 = the EXTRA edition; the round's chart while one is
     running, else what song select is set to */
  diff?: number;
  ed?: number;
  /* this console doesn't have the picker's current song */
  nosong?: boolean;
  connected: boolean;
  role: string;
  loaded: boolean;
  ready: boolean;
  reason: string;
  autoplay: boolean;
  autoplay_pct: number;
  autoplay_mode?: string;
  avatar: string | null;
  back_in_sel: boolean;
  score: number;
  combo: number;
  pct: number;
  life: number;
  /* the clear % the console's game uses for this song + difficulty
     (0 = not reported) */
  border: number;
  final: {
    score: number;
    pct: number;
    max_combo: number;
    life: number;
    jc: number[];
    border?: number;
    /* the console's verdict on the final state; null = old client */
    clear?: boolean | null;
    drop?: boolean;
    quit?: boolean;
  } | null;
  session: SessionStats;
}

/* room session ranking (server/lobby.py _apply_session_ranking) */
export interface SessionStats {
  pos: number; /* 1-based place: wins, then placement score */
  wins: number;
  score: number; /* summed normalized placements */
  places: [number, number, number]; /* times ranked 1st / 2nd / 3rd */
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
  border: number;
  clear: boolean | null;
  drop: boolean;
  /* the chart this player was on (see MemberSnap.diff) */
  diff?: number;
  ed?: number;
  plat?: Platform;
  autoplay: boolean;
  autoplay_pct: number;
  autoplay_mode?: string;
  avatar: string | null;
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
  /* room option: everyone plays the picker's difficulty */
  same_diff?: boolean;
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
  key?: string;
  on?: boolean;
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
