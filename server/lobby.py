#!/usr/bin/env python3
"""lobby.py - MegaMix LAN MP lobby/round engine. PURE logic, no asyncio.

Protocol v1 (NDJSON, "t" = type)
C->S: hello / ping (existing); set_name {name}; lobby_create {}; lobby_join {code};
      lobby_leave {}; kick {cid} (owner only); sel {pv, name} (leader, SELECT only);
      sel_ack {pv, has:bool, ok:bool, fb_pv:int (-1 none), fb_name:str};
      start_req {pv, name} (leader, SELECT only); launch_fail {round};
      loaded {round}; state {round, score, combo, pct, life, border};
      final {round, score, pct, max_combo, jc:[5], life, border, clear, drop,
             quit}; back_in_sel {round};
      (border = the clear % the console's game uses for its song+difficulty,
      0/absent unknown; clear = the console's own verdict on the FINAL state
      (pct >= border with life left, never after a pause-menu quit), absent
      on old clients; drop = all life lost; quit = left from the pause menu);
      scene {where: sel|results|game|other}; leader/owner back_in_sel in
      RESULTS ends the round for everyone (in PLAYING it ends when the
      standings complete); a member off song select at launch becomes a
      spectator; owner leaving hands ownership to the first connected
      non-bot member and rejoining within 120s restores it;
      avatar {module, chara, mh} (Shared Set A slot-1 module id, -1 unknown;
      chara = the module's character 0..9 (MIKU RIN LEN LUKA NERU HAKU KAITO
      MEIKO SAKINE TETO), -1 unknown; mh = signature of the sender's module
      table entry (FNV-1a of chara, cos, sort_index), 0 unknown; any time);
      avatar_get {img} (an opted-in console asks for a member's uploaded
      portrait; handled by mp_server, answered with avatar_px {img, rgba} or
      avatar_px {img, err});
      autoplay {on, pct, mode} (AutoPlay on/off, hit-rate 0..100, bot mode);
      avatar_img {module,w,h,rgba} (base64 portrait upload; handled by
      mp_server, not the engine);
      hello may carry plat: "switch"|"pc" (which version of the mod; when
      absent it is derived from mod/game, see platform_of);
      diff {d, e} (the console's song-select difficulty: d 0..3 = EASY
      NORMAL HARD EXTREME, e 1 = the EXTRA edition; any time, sent on
      change); loaded and final may carry diff/ed too (the chart the console
      actually plays this round, frozen for the board and standings);
      set_opt {key, value} (owner only; key same_diff: bool = every member
      must play the song picker's difficulty when their console has that
      chart).
S->C: welcome/error/pong (existing);
      lobby {code, phase, leader, you, round, sel_pv, same_diff,
             members:[{cid,name,connected,role,loaded,module,chara,mh,img,sr,
                       plat,diff,ed,nosong}]}
      (plat: switch|pc|bot; diff/ed: that member's difficulty, -1/0 unknown;
      nosong: the member's console doesn't have the picker's current song)
      (img: id of the member's uploaded portrait, "" none/hidden/relay off)
      (sr: session ranking [pos, wins, firsts, seconds, thirds], pos = 1-based
      place in the room's session leaderboard, see _apply_session_ranking;
      a member who leaves and rejoins keeps it until the lobby empties)
      (phase in SELECT/LOADING/PLAYING/RESULTS; role in player/spectator/dropped)
      lobby_left {reason: left|kicked|closed};
      lobby_error {code: not_found|already_in_lobby|not_in_lobby|not_leader|bad_phase, msg};
      sel {pv, name};
      sel_status {pv, missing:[{cid,name,fb_name|null}], cant:[{cid,name}]};
      launch {round, pv, name, players:[cid], spectators:[cid]};
      countdown {round, secs};
      start {round, dropped:[cid]};
      board {round, e:[{cid,name,score,pct,life,rank,connected,border,
                        diff,ed}]};
      event {kind, cid, name, value};
      (kinds: combo, low_life, drop_out (all life lost, once per round),
      joined, left, disconnected, reconnected, leader, owner,
      same_diff (the owner switched the room option, value 1/0));
      standings {round, complete:bool, e:[{cid,name,score,pct,max_combo,life,rank,final:bool,
                 border, clear:bool|null, drop:bool, diff, ed}]};
      (rank: tied rows share the LOWEST position of their group, 1-3-3-4);
      round_end {round, next_leader}.

Every public method appends (cid, msg_dict) to self.outbox; the caller drains
it and sends. `now` is always a float-seconds argument (testable clock).
"""

import random

CODE_CHARS = "ABCDEFGHJKLMNPQRSTUVWXYZ"
SELECT_DC_REMOVED_S = 60.0
SELECT_LEADER_HANDOFF_S = 10.0
LEADER_LOADED_START_S = 30.0
NO_LEADER_LOAD_S = 90.0
BOARD_MIN_INTERVAL_S = 0.1
ROUND_END_ALL_BACK_S = 90.0
PAUSE_VOTE_S = 15.0
PLAYING_ALL_DC_ABORT_S = 60.0


def _competition_ranks(rows):
    """Ranking on an already-sorted row list where tied rows share the
    LOWEST position of their group (1-3-3-4: two tied for first are both
    2nd); rows tie on (round(pct, 2), score)."""
    i = 0
    while i < len(rows):
        key = (round(rows[i]["pct"], 2), rows[i]["score"])
        j = i
        while (j + 1 < len(rows) and
               (round(rows[j + 1]["pct"], 2), rows[j + 1]["score"]) == key):
            j += 1
        for k in range(i, j + 1):
            rows[k]["rank"] = j + 1
        i = j + 1


def _session_placements(rows):
    """1-indexed placement P per row of an already-sorted row list; rows
    tied on (round(pct, 2), score) share the AVERAGE of their positions
    (two tied for 4th both get 4.5)."""
    out = []
    i = 0
    while i < len(rows):
        key = (round(rows[i]["pct"], 2), rows[i]["score"])
        j = i
        while (j + 1 < len(rows) and
               (round(rows[j + 1]["pct"], 2), rows[j + 1]["score"]) == key):
            j += 1
        out += [(i + 1 + j + 1) / 2.0] * (j - i + 1)
        i = j + 1
    return out


def platform_of(mod, game=None, plat=None):
    """Which version of the mod a client runs: "switch" (Mega Mix, a
    console or Ryujinx), "pc" (Mega Mix+) or "bot". An explicit hello
    `plat` wins; older clients are told apart by their mod / game strings
    (the PC mod says mod "pc-..." and game "mm+...")."""
    if plat in ("switch", "pc"):
        return plat
    mod = mod if isinstance(mod, str) else ""
    game = game if isinstance(game, str) else ""
    if mod.startswith("bot") or mod == "sim":
        return "bot"
    if mod.startswith("pc-") or game.startswith("mm+"):
        return "pc"
    return "switch"


def _diff_of(msg):
    """(d, e) from a message's d/e or diff/ed fields, None when absent or
    out of range (d 0..3, e 0..1; e only exists on EXTREME)."""
    d = msg.get("d", msg.get("diff"))
    e = msg.get("e", msg.get("ed", 0))
    if isinstance(d, bool) or not isinstance(d, int) or not 0 <= d <= 3:
        return None
    if isinstance(e, bool) or not isinstance(e, int) or not 0 <= e <= 1:
        e = 0
    return (d, e if d == 3 else 0)


def _f(v, default=0.0):
    """number from a client message (bools and junk -> default)"""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return default
    return float(v)


class Member:
    __slots__ = ("cid", "name", "connected", "role", "loaded", "final",
                 "back_in_sel", "state", "prev_combo_bucket", "low_life_armed",
                 "disconnect_at", "last_activity",
                 "ready", "ready_reason", "ready_pv", "autoplay",
                 "autoplay_pct", "autoplay_mode", "scene",
                 "pause_req_at", "pause_req_kind",
                 "life_seen", "drop_sent",
                 "wins", "placement_score", "places", "rdiff")

    def __init__(self, cid, name):
        self.cid = cid
        self.name = name
        self.connected = True
        self.role = "player"        # player | spectator | dropped
        self.loaded = False
        self.final = None           # final dict or None
        self.back_in_sel = False
        self.state = {"score": 0, "combo": 0, "pct": 0.0, "life": 127}
        self.prev_combo_bucket = 0
        self.low_life_armed = True
        self.disconnect_at = None
        self.last_activity = None
        self.ready = True
        self.ready_reason = ""
        self.ready_pv = -1
        self.autoplay = False
        self.autoplay_pct = 100
        self.autoplay_mode = ""      # "auto"|"pro"|"97"|"streak"|"human"|"clutch"|"tie"|"custom"
        self.scene = None         # sel|results|game|other, None = unknown ("sel")
        self.pause_req_at = 0.0
        self.pause_req_kind = ""
        self.life_seen = False      # this round reported life > 0 once
        self.drop_sent = False      # drop_out event sent this round
        # lobby session ranking (late joiners start at zero)
        self.wins = 0
        self.placement_score = 0.0
        self.places = [0, 0, 0]     # times ranked 1st / 2nd / 3rd
        self.rdiff = None           # (d, e) played this round, None unknown


class Round:
    __slots__ = ("id", "pv", "name", "leader", "players", "spectators",
                 "dropped", "launch_at", "leader_loaded_at", "started",
                 "start_sent", "countdown_at",
                 "first_final_at", "complete_at", "complete_announced",
                 "leader_back", "ranked")

    def __init__(self, rid, pv, name, leader):
        self.id = rid
        self.pv = pv
        self.name = name
        self.leader = leader
        self.players = []
        self.spectators = []
        self.dropped = []
        self.launch_at = None
        self.leader_loaded_at = None
        self.started = False
        self.start_sent = False    # real `start` broadcast happened
        self.countdown_at = None   # when `start` may go out (0 = sent now)
        self.first_final_at = None
        self.complete_at = None
        self.complete_announced = False
        self.leader_back = False
        self.ranked = False        # session ranking applied


class Lobby:
    def __init__(self, code):
        self.code = code
        self.phase = "SELECT"       # SELECT | LOADING | PLAYING | RESULTS
        self.members = []           # join order
        self.leader_cid = None
        self.owner_cid = None
        self.pick_mode = "rotate"   # owner | assigned | rotate
        self.assigned_cid = None
        self.pending_pick_mode = None   # (mode, cid) applied at round_end
        self.same_diff = False      # owner option: play the picker's difficulty
        self.group_paused = False
        self.vote = None   # {"kind","agreed":set,"ends_at":float}
        self.sel_pv = None
        self.sel_name = None
        self.acks = {}              # cid -> ack dict for current sel_pv
        self._sel_status_sent = None  # last (missing,cant) signature sent to leader
        self._nosong_sent = ()        # cids flagged nosong in the last lobby msg
        self.seq = 0
        self.round = None
        self.pending_leader = None
        self.last_board_at = 0.0
        self.board_dirty = False
        self.prev_owner = None    # (cid, deadline) rejoin grace
        # cid -> (wins, placement_score, places) of members who left; given
        # back when they rejoin, gone with the lobby once it empties
        self.session_kept = {}

    def effective_leader(self):
        if self.pick_mode == "owner":
            return self.owner_cid
        if self.pick_mode == "assigned" and self.assigned_cid in                 {m.cid for m in self.members}:
            return self.assigned_cid
        return self.leader_cid

    def leader(self):
        return self.effective_leader()


class LobbyManager:
    def __init__(self, log=None, countdown=0.0):
        self.countdown = countdown
        self.lobbies = {}           # code -> Lobby
        self.by_cid = {}            # cid -> Lobby
        self.outbox = []            # (cid, msg)
        self.log = log or (lambda s: None)
        self.max_members = 8        # real consoles cap at 8; raise for demos
        self._rng = random.Random()
        self._client_names = {}     # cid -> display name (all clients)
        self._client_modules = {}   # cid -> avatar module id (all clients)
        self._client_avatar = {}    # cid -> (chara, mh) of that module
        self._client_plat = {}      # cid -> "switch" | "pc" | "bot"
        self._client_diff = {}      # cid -> (d, e) song-select difficulty
        # cid -> portrait id consoles may request ("" none); set by mp_server
        self.avatar_img_of = lambda cid: ""
        self._bots = set()          # cids whose mod starts with "bot"

    # ------------------------------------------------- helpers
    def _lobby_of(self, cid):
        return self.by_cid.get(cid)

    def _member(self, lobby, cid):
        if lobby is None:
            return None
        for m in lobby.members:
            if m.cid == cid:
                return m
        return None

    def _send(self, cid, msg):
        self.outbox.append((cid, msg))

    def _broadcast(self, lobby, msg, skip=None):
        for m in lobby.members:
            if m.cid != skip:
                self._send(m.cid, msg)

    @staticmethod
    def session_order(lobby):
        """Members by session ranking: wins, then placement score (both
        descending), join order breaking exact ties."""
        return sorted(lobby.members,
                      key=lambda m: (-m.wins, -m.placement_score))

    def session_pos(self, lobby):
        """cid -> 1-based session leaderboard position"""
        return {m.cid: i + 1 for i, m in enumerate(self.session_order(lobby))}

    def nosong(self, lobby):
        """cids (join order) whose console reported that it doesn't have
        the picker's current song"""
        if lobby.sel_pv is None:
            return ()
        lead = lobby.leader()
        return tuple(m.cid for m in lobby.members
                     if m.cid != lead and m.cid in lobby.acks and
                     not lobby.acks[m.cid]["has"])

    def diff_of(self, lobby, m):
        """(d, e) to show for a member: the chart of the running round
        once known, else the console's song-select difficulty; (-1, 0)
        unknown"""
        if lobby.round is not None and m.rdiff is not None:
            return m.rdiff
        return self._client_diff.get(m.cid, (-1, 0))

    def _lobby_msg(self, lobby, you_cid):
        pos = self.session_pos(lobby)
        nosong = self.nosong(lobby)
        lobby._nosong_sent = nosong
        return {"t": "lobby", "code": lobby.code, "phase": lobby.phase,
                "leader": lobby.leader(), "you": you_cid,
                "owner": lobby.owner_cid, "pick_mode": lobby.pick_mode,
                "assigned": lobby.assigned_cid,
                "same_diff": lobby.same_diff,
                "group_paused": lobby.group_paused,
                "round": lobby.round.id if lobby.round else 0,
                "sel_pv": lobby.sel_pv,
                "members": [{"cid": m.cid, "name": m.name,
                             "connected": m.connected, "role": m.role,
                             "loaded": m.loaded,
                             "ready": m.ready, "reason": m.ready_reason,
                             "autoplay": m.autoplay,
                             "autoplay_pct": m.autoplay_pct,
                             "autoplay_mode": m.autoplay_mode,
                             "module": self._client_modules.get(m.cid, -1),
                             "chara": self._client_avatar.get(m.cid, (-1, 0))[0],
                             "mh": self._client_avatar.get(m.cid, (-1, 0))[1],
                             "img": self.avatar_img_of(m.cid),
                             "sr": [pos[m.cid], m.wins] + m.places,
                             "plat": self._client_plat.get(m.cid, "switch"),
                             "diff": self.diff_of(lobby, m)[0],
                             "ed": self.diff_of(lobby, m)[1],
                             "nosong": m.cid in nosong}
                            for m in lobby.members]}

    def _broadcast_lobby(self, lobby):
        for m in lobby.members:
            self._send(m.cid, self._lobby_msg(lobby, m.cid))

    def _event(self, lobby, kind, cid, name, value=None):
        self._broadcast(lobby, {"t": "event", "kind": kind, "cid": cid,
                                "name": name, "value": value})

    def _error(self, cid, code, msg):
        self._send(cid, {"t": "lobby_error", "code": code, "msg": msg})

    def _new_code(self):
        while True:
            code = "".join(self._rng.choice(CODE_CHARS) for _ in range(4))
            if code not in self.lobbies:
                return code

    # ------------------------------------------------- connection events
    def on_hello(self, cid, name, mod, now, plat=None, game=None):
        self._client_names[cid] = name
        self._client_plat[cid] = platform_of(mod, game, plat)
        if mod and mod.startswith("bot"):
            self._bots.add(cid)
        else:
            self._bots.discard(cid)
        lobby = self._lobby_of(cid)
        if lobby is None:
            return
        m = self._member(lobby, cid)
        if m is None:
            return
        m.connected = True
        m.disconnect_at = None
        m.name = name
        self._event(lobby, "reconnected", cid, name)
        self._send(cid, self._lobby_msg(lobby, cid))
        if lobby.phase == "SELECT" and lobby.sel_pv is not None and m.cid != lobby.leader():
            self._send(cid, {"t": "sel", "pv": lobby.sel_pv,
                             "name": lobby.sel_name})
        r = lobby.round
        if r is not None:
            self._send(cid, {"t": "launch", "round": r.id, "pv": r.pv,
                             "name": r.name, "players": r.players,
                             "spectators": r.spectators})
            if r.started and r.start_sent:
                self._send(cid, {"t": "start", "round": r.id,
                                 "dropped": r.dropped})
            self._send_board(lobby)

    def on_disconnect(self, cid, now):
        lobby = self._lobby_of(cid)
        if lobby is None:
            return
        m = self._member(lobby, cid)
        if m is None or not m.connected:
            return
        m.connected = False
        m.disconnect_at = now
        self._event(lobby, "disconnected", cid, m.name)
        self._broadcast_lobby(lobby)
        if lobby.phase in ("PLAYING", "RESULTS"):
            self._send_standings(lobby, now)
            self._check_complete(lobby, now)

    # ------------------------------------------------- message dispatch
    def on_message(self, cid, msg, now):
        t = msg.get("t")
        if t == "set_name":
            self._set_name(cid, msg.get("name", ""), now)
        elif t == "lobby_create":
            self._create(cid, now)
        elif t == "lobby_join":
            self._join(cid, msg.get("code", ""), now)
        elif t == "lobby_leave":
            self._leave(cid, "left", now)
        elif t == "sel":
            self._sel(cid, msg, now)
        elif t == "sel_ack":
            self._sel_ack(cid, msg, now)
        elif t == "start_req":
            self._start_req(cid, msg, now)
        elif t == "launch_fail":
            self._launch_fail(cid, msg, now)
        elif t == "loaded":
            self._loaded(cid, msg, now)
        elif t == "state":
            self._state(cid, msg, now)
        elif t == "final":
            self._final(cid, msg, now)
        elif t == "back_in_sel":
            self._back_in_sel(cid, msg, now)
        elif t == "scene":
            self._scene(cid, msg, now)
        elif t == "ready":
            self._ready(cid, msg, now)
        elif t == "set_pick_mode":
            self._set_pick_mode(cid, msg, now)
        elif t == "set_opt":
            self._set_opt(cid, msg, now)
        elif t == "diff":
            self._set_diff(cid, msg, now)
        elif t == "kick":
            self._kick(cid, msg.get("cid"), now)
        elif t == "autoplay":
            # autoplay {on, pct, mode}: on/off flag, optional hit-rate 0..100
            # and optional bot-mode name (auto|pro|97|streak|human|clutch|tie|custom)
            m = self._member(self._lobby_of(cid), cid)
            if m:
                m.autoplay = bool(msg.get("on"))
                pct = msg.get("pct")
                m.autoplay_pct = (max(0, min(100, int(pct)))
                                  if isinstance(pct, int)
                                  and not isinstance(pct, bool)
                                  else 100)
                mode = msg.get("mode")
                if isinstance(mode, str):
                    m.autoplay_mode = mode[:16]
                lobby = self._lobby_of(cid)
                if lobby:
                    self._broadcast_lobby(lobby)
        elif t in ("pause_req", "resume_req"):
            self._pause_req(cid, msg, now, t == "pause_req")
        elif t == "avatar":
            self._set_avatar(cid, msg.get("module"), msg.get("chara", -1),
                             msg.get("mh", 0))

    @staticmethod
    def _int_in(v, lo, hi):
        return isinstance(v, int) and not isinstance(v, bool) and lo <= v <= hi

    def _set_avatar(self, cid, module, chara=-1, mh=0):
        if not self._int_in(module, -1, 99999):
            self._error(cid, "bad_avatar", "module must be an int in -1..99999")
            return
        if not self._int_in(chara, -1, 15):
            chara = -1
        if not self._int_in(mh, 0, 0xFFFFFFFF):
            mh = 0
        if (self._client_modules.get(cid, -1) == module and
                self._client_avatar.get(cid, (-1, 0)) == (chara, mh)):
            return
        self._client_modules[cid] = module
        self._client_avatar[cid] = (chara, mh)
        self.touch_avatar(cid)

    def touch_avatar(self, cid):
        """Resend the lobby of `cid` (its avatar/portrait changed)."""
        lobby = self._lobby_of(cid)
        if lobby:
            self._broadcast_lobby(lobby)

    def _set_name(self, cid, name, now):
        if name:
            self._client_names[cid] = name
        lobby = self._lobby_of(cid)
        if lobby is None:
            return
        m = self._member(lobby, cid)
        if m and name:
            m.name = name
            self._broadcast_lobby(lobby)

    # ------------------------------------------------- lobby lifecycle
    def _create(self, cid, now):
        if self._lobby_of(cid) is not None:
            self._error(cid, "already_in_lobby", "already in a lobby")
            return
        lobby = Lobby(self._new_code())
        lobby.leader_cid = cid
        lobby.owner_cid = cid
        lobby.members.append(Member(cid, self._name_of(cid)))
        self.lobbies[lobby.code] = lobby
        self.by_cid[cid] = lobby
        self.log(f"lobby {lobby.code} created by {cid}")
        self._send(cid, self._lobby_msg(lobby, cid))

    def _name_of(self, cid):
        return self._client_names.get(cid, cid[:8])

    def _join(self, cid, code, now):
        if self._lobby_of(cid) is not None:
            self._error(cid, "already_in_lobby", "already in a lobby")
            return
        lobby = self.lobbies.get(code)
        if lobby is None:
            self._error(cid, "not_found", "no such lobby")
            return
        if len(lobby.members) >= self.max_members:
            self._error(cid, "full", "lobby is full")
            return
        m = Member(cid, self._name_of(cid))
        kept = lobby.session_kept.pop(cid, None)
        if kept is not None:
            m.wins, m.placement_score, m.places = kept[0], kept[1], list(kept[2])
        m.role = "player" if lobby.phase == "SELECT" else "spectator"
        lobby.members.append(m)
        self.by_cid[cid] = lobby
        if (lobby.prev_owner is not None and
                lobby.prev_owner[0] == cid and now < lobby.prev_owner[1]):
            lobby.owner_cid = cid
            lobby.prev_owner = None
            self._event(lobby, "owner", cid, m.name)
        self.log(f"lobby {lobby.code}: {cid} joined")
        self._broadcast_lobby(lobby)
        self._event(lobby, "joined", cid, m.name)
        if lobby.phase == "SELECT" and lobby.sel_pv is not None:
            self._send(cid, {"t": "sel", "pv": lobby.sel_pv,
                             "name": lobby.sel_name})

    def _leave(self, cid, reason, now):
        lobby = self._lobby_of(cid)
        if lobby is None:
            self._error(cid, "not_in_lobby", "not in a lobby")
            return
        self._remove_member(lobby, cid, reason, now)

    def _kick(self, cid, target, now):
        lobby = self._lobby_of(cid)
        if lobby is None:
            self._error(cid, "not_in_lobby", "not in a lobby")
            return
        if lobby.owner_cid != cid:
            self._error(cid, "not_owner", "only the owner can kick")
            return
        if target == cid or self._member(lobby, target) is None:
            self._error(cid, "bad_member", "kick target must be another member")
            return
        self.log(f"lobby {lobby.code}: {target} kicked by owner {cid}")
        self._remove_member(lobby, target, "kicked", now)

    def _remove_member(self, lobby, cid, reason, now):
        idx = next((i for i, m in enumerate(lobby.members) if m.cid == cid), -1)
        if idx < 0:
            return
        was_leader = lobby.leader() == cid
        m = lobby.members.pop(idx)
        lobby.session_kept[cid] = (m.wins, m.placement_score, list(m.places))
        self.by_cid.pop(cid, None)
        self._send(cid, {"t": "lobby_left", "reason": reason})
        if not lobby.members:
            del self.lobbies[lobby.code]
            self.log(f"lobby {lobby.code} closed (empty)")
            return
        if lobby.owner_cid == cid:
            lobby.prev_owner = (cid, now + 120.0)
            non_bots = [m for m in lobby.members
                        if m.cid not in self._bots]
            new_owner = (next((m for m in non_bots if m.connected), None) or
                         (non_bots[0] if non_bots else lobby.members[0]))
            lobby.owner_cid = new_owner.cid
            self._event(lobby, "owner", lobby.owner_cid, new_owner.name)
        if was_leader:
            new_leader = lobby.members[idx % len(lobby.members)]
            lobby.leader_cid = new_leader.cid
            self._event(lobby, "leader", new_leader.cid, new_leader.name)
            self.log(f"lobby {lobby.code}: leader -> {new_leader.cid}")
        self._event(lobby, "left", cid, m.name)
        self._broadcast_lobby(lobby)

    # ------------------------------------------------- SELECT phase
    def _sel(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None:
            self._error(cid, "not_in_lobby", "not in a lobby")
            return
        if lobby.leader() != cid:
            self._error(cid, "not_leader", "only the leader can select")
            return
        if lobby.phase != "SELECT":
            self._error(cid, "bad_phase", "not in SELECT phase")
            return
        lobby.sel_pv = msg.get("pv")
        lobby.sel_name = msg.get("name")
        lobby.acks = {}
        lobby._sel_status_sent = None
        self.log(f"lobby {lobby.code}: sel pv={lobby.sel_pv} '{lobby.sel_name}'")
        self._broadcast(lobby, {"t": "sel", "pv": lobby.sel_pv,
                                "name": lobby.sel_name}, skip=cid)
        if lobby._nosong_sent:          # the flags belonged to the old pick
            self._broadcast_lobby(lobby)

    def _sel_ack(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.sel_pv is None:
            return
        if msg.get("pv") != lobby.sel_pv:
            return
        lobby.acks[cid] = {
            "has": bool(msg.get("has")), "ok": bool(msg.get("ok")),
            "fb_pv": msg.get("fb_pv", -1), "fb_name": msg.get("fb_name"),
        }
        # aggregate -> sel_status to leader when changed
        missing, cant = [], []
        for m in lobby.members:
            if m.cid == lobby.leader():
                continue
            a = lobby.acks.get(m.cid)
            if a and not a["has"]:
                missing.append({"cid": m.cid, "name": m.name,
                                "fb_name": a.get("fb_name")})
            if a and a["has"] and not a["ok"]:
                cant.append({"cid": m.cid, "name": m.name})
        sig = (tuple(x["cid"] for x in missing), tuple(x["cid"] for x in cant))
        if sig != lobby._sel_status_sent:
            lobby._sel_status_sent = sig
            self._send_sel_status(lobby, missing, cant)
        if self.nosong(lobby) != lobby._nosong_sent:
            self._broadcast_lobby(lobby)    # everyone sees who lacks the song

    def _send_sel_status(self, lobby, missing=None, cant=None):
        if missing is None or cant is None:
            missing, cant = [], []
            for m in lobby.members:
                if m.cid == lobby.leader():
                    continue
                a = lobby.acks.get(m.cid)
                if a and not a["has"]:
                    missing.append({"cid": m.cid, "name": m.name,
                                    "fb_name": a.get("fb_name")})
                if a and a["has"] and not a["ok"]:
                    cant.append({"cid": m.cid, "name": m.name})
        not_ready = [{"cid": m.cid, "name": m.name,
                      "reason": m.ready_reason or "not_ready"}
                     for m in lobby.members
                     if m.cid != lobby.leader() and m.connected and
                     not m.ready]
        self._send(lobby.leader(), {"t": "sel_status", "pv": lobby.sel_pv,
                                    "missing": missing, "cant": cant,
                                    "not_ready": not_ready})

    def _apply_pick_mode(self, lobby, mode, cid):
        lobby.pick_mode = mode
        lobby.assigned_cid = cid if mode == "assigned" else None
        if mode != "assigned":
            lobby.assigned_cid = None
        if mode == "owner":
            lobby.leader_cid = lobby.owner_cid
        elif mode == "assigned" and lobby.assigned_cid:
            lobby.leader_cid = lobby.assigned_cid
        elif mode == "rotate":
            pass   # keep current leader; rotation applies at round_end
        lobby.pending_pick_mode = None
        self._broadcast_lobby(lobby)
        self._send_sel_status(lobby) if lobby.sel_pv is not None else None
        cur = lobby.leader()
        if cur:
            self._event(lobby, "leader", cur,
                        self._member(lobby, cur).name
                        if self._member(lobby, cur) else cur)

    def _set_pick_mode(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None:
            self._error(cid, "not_in_lobby", "not in a lobby")
            return
        if lobby.owner_cid != cid:
            self._error(cid, "not_owner", "only the owner can set pick mode")
            return
        mode = msg.get("mode")
        if mode not in ("owner", "assigned", "rotate"):
            self._error(cid, "bad_mode", "mode must be owner|assigned|rotate")
            return
        assignee = msg.get("cid")
        if mode == "assigned" and (assignee is None or
                                   self._member(lobby, assignee) is None):
            self._error(cid, "bad_member", "assigned cid must be a member")
            return
        if lobby.phase == "SELECT":
            self._apply_pick_mode(lobby, mode, assignee)
        else:
            lobby.pending_pick_mode = (mode, assignee)
            self._broadcast_lobby(lobby)

    def _set_opt(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None:
            self._error(cid, "not_in_lobby", "not in a lobby")
            return
        if lobby.owner_cid != cid:
            self._error(cid, "not_owner", "only the owner can set room options")
            return
        key, value = msg.get("key"), msg.get("value")
        if key != "same_diff" or not isinstance(value, bool):
            self._error(cid, "bad_opt", "unknown option or value")
            return
        if lobby.same_diff == value:
            return
        lobby.same_diff = value
        self.log(f"lobby {lobby.code}: same_diff={value}")
        self._broadcast_lobby(lobby)
        m = self._member(lobby, cid)
        self._event(lobby, "same_diff", cid, m.name if m else cid,
                    1 if value else 0)

    def _set_diff(self, cid, msg, now):
        de = _diff_of(msg)
        if de is None or self._client_diff.get(cid) == de:
            return
        self._client_diff[cid] = de
        lobby = self._lobby_of(cid)
        if lobby:
            self._broadcast_lobby(lobby)

    def _pause_req(self, cid, msg, now, pause):
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.round is None:
            return
        if msg.get("round") != lobby.round.id:
            return
        if cid not in lobby.round.players:
            return
        m = self._member(lobby, cid)
        if m is not None and m.final is not None:
            return                     # already finished: no say in pause
        kind = "pause" if pause else "resume"
        if lobby.group_paused == pause:
            # already in the requested state: resync the requester only
            self._send(cid, {"t": kind, "by": "", "sync": True})
            return
        if (m is not None and m.pause_req_kind == kind and
                now - m.pause_req_at < 0.5):
            return                     # same kind repeated within 0.5 s
        if m is not None:
            m.pause_req_at = now
            m.pause_req_kind = kind
        if lobby.owner_cid == cid:
            # owner: immediate
            lobby.vote = None
            lobby.group_paused = pause
            self._broadcast(lobby, {"t": kind, "by": cid})
            self.log(f"lobby {lobby.code}: {kind} by owner {cid}")
            if not pause:
                self._bump_activity(lobby, now)
            return
        # vote: single open vote; a req while open counts as agreeing
        v = lobby.vote
        if v is None or v["kind"] != kind or now >= v["ends_at"]:
            v = {"kind": kind, "agreed": {cid},
                 "ends_at": now + PAUSE_VOTE_S}
            lobby.vote = v
            self._send_pause_vote(lobby, now)
        else:
            prev = len(v["agreed"])
            v["agreed"].add(cid)
            if len(v["agreed"]) > prev:   # agreed set grew
                self._send_pause_vote(lobby, now)
        need = {c for c in lobby.round.players
                if self._member(lobby, c) is not None and
                self._member(lobby, c).connected and
                self._member(lobby, c).final is None}
        if v["agreed"] >= need and len(need) > 0:
            lobby.vote = None
            lobby.group_paused = pause
            self._broadcast(lobby, {"t": kind, "by": cid})
            self.log(f"lobby {lobby.code}: {kind} vote passed")
            if not pause:
                self._bump_activity(lobby, now)

    def _bump_activity(self, lobby, now):
        r = lobby.round
        if r is None:
            return
        for p in r.players:
            pm = self._member(lobby, p)
            if pm is not None and pm.last_activity is not None:
                pm.last_activity = now

    def _send_pause_vote(self, lobby, now, expired=False):
        v = lobby.vote
        if v is None:
            return
        need = sum(1 for c in lobby.round.players
                   if self._member(lobby, c) is not None and
                   self._member(lobby, c).connected and
                   self._member(lobby, c).final is None)
        msg = {"t": "pause_vote", "kind": v["kind"],
               "agreed": sorted(v["agreed"]), "need": need,
               "ends_in": max(0, int(v["ends_at"] - now))}
        if expired:
            msg["expired"] = True
        self._broadcast(lobby, msg)

    def _scene(self, cid, msg, now):
        m = self._member(self._lobby_of(cid), cid)
        if m is None:
            return
        w = msg.get("where")
        if w in ("sel", "results", "game", "other"):
            m.scene = w

    def _ready(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None:
            return
        m = self._member(lobby, cid)
        if m is None:
            return
        m.ready = bool(msg.get("ready"))
        m.ready_reason = msg.get("reason", "") or ""
        m.ready_pv = msg.get("pv", -1)
        if lobby.phase == "SELECT" and lobby.sel_pv is not None:
            self._send_sel_status(lobby)

    # ------------------------------------------------- round start
    def _start_req(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None:
            self._error(cid, "not_in_lobby", "not in a lobby")
            return
        if lobby.leader() != cid:
            self._error(cid, "not_leader", "only the leader can start")
            return
        if lobby.phase != "SELECT":
            self._error(cid, "bad_phase", "not in SELECT phase")
            return
        lobby.seq += 1
        r = Round(lobby.seq, msg.get("pv", lobby.sel_pv),
                  msg.get("name", lobby.sel_name), lobby.leader())
        players, spectators = [], []
        for m in lobby.members:
            a = lobby.acks.get(m.cid)
            if m.cid == lobby.leader():
                players.append(m.cid)
            elif not m.connected:
                spectators.append(m.cid)
            elif (m.scene or "sel") != "sel":
                # away from song select can't join the round
                spectators.append(m.cid)
            elif a is None:                                  # optimistic no-ack
                players.append(m.cid)
            elif a["has"] and a["ok"]:
                players.append(m.cid)
            elif not a["has"] and a.get("fb_pv", -1) >= 0:
                players.append(m.cid)
            else:
                spectators.append(m.cid)
        r.players, r.spectators = players, spectators
        r.launch_at = now
        for m in lobby.members:
            m.role = ("player" if m.cid in players else "spectator")
            m.loaded = False
            m.final = None
            m.back_in_sel = False
            m.prev_combo_bucket = 0
            m.low_life_armed = True
            m.life_seen = False
            m.drop_sent = False
            m.rdiff = None
            m.state = {"score": 0, "combo": 0, "pct": 0.0, "life": 127}
        lobby.round = r
        lobby.phase = "LOADING"
        self.log(f"lobby {lobby.code}: launch r{r.id} pv={r.pv} "
                 f"players={players} spec={spectators}")
        self._broadcast(lobby, {"t": "launch", "round": r.id, "pv": r.pv,
                                "name": r.name, "players": players,
                                "spectators": spectators})
        self._broadcast_lobby(lobby)

    def _launch_fail(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.round is None:
            return
        if msg.get("round") != lobby.round.id:
            return
        m = self._member(lobby, cid)
        if m is None or m.role != "player":
            return
        m.role = "spectator"
        if cid in lobby.round.players:
            lobby.round.players.remove(cid)
        lobby.round.spectators.append(cid)
        self._broadcast_lobby(lobby)

    def _loaded(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.round is None:
            return
        r = lobby.round
        if msg.get("round") != r.id or r.started:
            return
        m = self._member(lobby, cid)
        if m is None or m.role != "player":
            return
        m.loaded = True
        # the chart this member plays: what the console says it loaded,
        # else its song-select difficulty at this moment (it can change
        # that again once it is back on song select with the results up)
        m.rdiff = _diff_of(msg) or self._client_diff.get(cid)
        if cid == r.leader:
            r.leader_loaded_at = now
        self._broadcast_lobby(lobby)
        if all(self._member(lobby, p) and self._member(lobby, p).loaded
               for p in r.players):
            self._do_start(lobby, now, [])

    # ------------------------------------------------- START
    def _do_start(self, lobby, now, drop_cids):
        r = lobby.round
        for d in drop_cids:
            m = self._member(lobby, d)
            if m and m.role == "player":
                m.role = "dropped"
                if d in r.players:
                    r.players.remove(d)
                if d not in r.dropped:
                    r.dropped.append(d)
        r.started = True
        lobby.phase = "PLAYING"
        lobby.last_board_at = 0.0
        lobby.board_dirty = False
        self.log(f"lobby {lobby.code}: start r{r.id} dropped={r.dropped}")
        if self.countdown > 0:
            r.countdown_at = now + self.countdown
            self._broadcast(lobby, {"t": "countdown", "round": r.id,
                                    "secs": self.countdown})
        else:
            r.start_sent = True
            self._broadcast(lobby, {"t": "start", "round": r.id,
                                    "dropped": r.dropped})
        self._broadcast_lobby(lobby)

    # ------------------------------------------------- PLAYING
    def _state(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.phase != "PLAYING" or lobby.round is None:
            return
        r = lobby.round
        if msg.get("round") != r.id or cid not in r.players:
            return
        m = self._member(lobby, cid)
        if m is None or m.final is not None:
            return                       # finished players send no more state
        score = msg.get("score", 0)
        combo = msg.get("combo", 0)
        pct = msg.get("pct", 0.0)
        life = msg.get("life", 0)
        m.last_activity = now
        prev_bucket = m.prev_combo_bucket
        prev_life = m.state.get("life", 0)
        border = _f(msg.get("border"), m.state.get("border", 0.0))
        m.state = {"score": score, "combo": combo, "pct": pct, "life": life,
                   "border": border}
        # a combo break re-arms the buckets: 50/100/... fire again
        if combo < m.prev_combo_bucket * 50:
            m.prev_combo_bucket = combo // 50
            prev_bucket = m.prev_combo_bucket
        if combo >= 50 and combo // 50 > prev_bucket:
            m.prev_combo_bucket = combo // 50
            self._event(lobby, "combo", cid, m.name, combo)
        if life <= 40 and prev_life > 40 and m.low_life_armed:
            m.low_life_armed = False
            self._event(lobby, "low_life", cid, m.name, life)
        elif life > 60:
            m.low_life_armed = True
        if _f(life) > 0:
            m.life_seen = True
        elif m.life_seen:
            self._drop_out(lobby, m)
        lobby.board_dirty = True

    def _drop_out(self, lobby, m):
        """all life lost: one drop_out event per member and round"""
        if m.drop_sent:
            return
        m.drop_sent = True
        self.log(f"lobby {lobby.code}: {m.cid} dropped out")
        self._event(lobby, "drop_out", m.cid, m.name)

    def _send_board(self, lobby):
        r = lobby.round
        if r is None:
            return
        e = []
        for p in r.players:
            m = self._member(lobby, p)
            if m is None:
                continue
            e.append({"cid": m.cid, "name": m.name,
                      "score": m.state.get("score", 0),
                      "pct": m.state.get("pct", 0.0),
                      "life": m.state.get("life", 0),
                      "connected": m.connected,
                      "border": m.state.get("border", 0.0),
                      "diff": self.diff_of(lobby, m)[0],
                      "ed": self.diff_of(lobby, m)[1]})
        e.sort(key=lambda x: (-x["pct"], -x["score"]))
        _competition_ranks(e)
        self._broadcast(lobby, {"t": "board", "round": r.id, "e": e})

    # ------------------------------------------------- RESULTS
    def _final(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.round is None:
            return
        r = lobby.round
        if msg.get("round") != r.id or cid not in r.players + r.dropped:
            return
        m = self._member(lobby, cid)
        if m is None:
            return
        clear = msg.get("clear")
        m.final = {"score": msg.get("score", 0), "pct": msg.get("pct", 0.0),
                   "max_combo": msg.get("max_combo", 0),
                   "jc": msg.get("jc", []), "life": msg.get("life", 0),
                   "border": _f(msg.get("border"),
                                m.state.get("border", 0.0)),
                   # the console's verdict; None = an old client sent none
                   "clear": clear if isinstance(clear, bool) else None,
                   "drop": msg.get("drop") is True,
                   "quit": msg.get("quit") is True}
        m.last_activity = now
        m.state["score"] = m.final["score"]
        m.state["pct"] = m.final["pct"]
        m.rdiff = _diff_of(msg) or m.rdiff or self._client_diff.get(cid)
        if m.final["drop"]:
            self._drop_out(lobby, m)
        if r.first_final_at is None and lobby.phase == "PLAYING":
            r.first_final_at = now
            self.log(f"lobby {lobby.code}: r{r.id} first final")
        self._send_standings(lobby, now)
        self._check_complete(lobby, now)

    def _standings(self, lobby, now):
        r = lobby.round
        rows = []
        for p in r.players:
            m = self._member(lobby, p)
            if m is None:
                continue
            f = m.final
            stale = (r.first_final_at is not None and
                     m.last_activity is not None and
                     now - m.last_activity >= 30.0 and f is None and
                     m.connected and not lobby.group_paused)
            rows.append({"cid": m.cid, "name": m.name,
                         "score": f["score"] if f else m.state.get("score", 0),
                         "pct": f["pct"] if f else m.state.get("pct", 0.0),
                         "max_combo": f["max_combo"] if f else 0,
                         "life": f["life"] if f else 0,
                         "final": f is not None or not m.connected or stale,
                         "connected": m.connected,
                         "border": (f["border"] if f else
                                    m.state.get("border", 0.0)),
                         "clear": f["clear"] if f else None,
                         "drop": bool(f and f["drop"]),
                         "diff": self.diff_of(lobby, m)[0],
                         "ed": self.diff_of(lobby, m)[1]})
        rows.sort(key=lambda x: (-x["pct"], -x["score"]))
        _competition_ranks(rows)
        complete = all(row["final"] for row in rows) if rows else True
        return rows, complete

    def _send_standings(self, lobby, now):
        r = lobby.round
        rows, complete = self._standings(lobby, now)
        self._broadcast(lobby, {"t": "standings", "round": r.id,
                                "complete": complete, "e": rows})
        if complete and r.complete_at is None:
            r.complete_at = now

    def _check_complete(self, lobby, now):
        """PLAYING -> RESULTS once every player finished (final, dc, or stale)."""
        r = lobby.round
        if r is None or lobby.phase != "PLAYING" or r.first_final_at is None:
            return
        _, complete = self._standings(lobby, now)
        if complete:
            lobby.phase = "RESULTS"
            self.log(f"lobby {lobby.code}: r{r.id} standings complete")
            if self._apply_session_ranking(lobby, now):
                self._broadcast_lobby(lobby)
            self._send_standings(lobby, now)
            if r.leader_back:
                self.log(f"lobby {lobby.code}: r{r.id} leader back, "
                         f"round ended with last final")
                self._round_end(lobby, now)

    def _apply_session_ranking(self, lobby, now):
        """Once per round, when its standings complete: N = the round's
        players (members who left mid-round included, placed last with a
        0.0 score); for P = 1-indexed placement (ties share the average
        position) wins += (P == 1), placement_score += (N - P) / (N - 1),
        and the shown rank (1-3-3-4, as on the results) counts a 1st/2nd/
        3rd place. Rounds with fewer than 2 players don't count. Leavers'
        stats are kept (session_kept) and restored if they rejoin before
        the lobby empties.
        Returns True if anything was applied."""
        r = lobby.round
        if r is None or r.ranked or not r.started:
            return False
        r.ranked = True
        rows, _ = self._standings(lobby, now)
        leavers = [c for c in r.players if self._member(lobby, c) is None]
        n = len(rows) + len(leavers)
        if n < 2:
            return False
        for row, p in zip(rows, _session_placements(rows)):
            m = self._member(lobby, row["cid"])
            if p == 1:
                m.wins += 1
            m.placement_score += (n - p) / (n - 1)
            if row["rank"] <= 3:
                m.places[row["rank"] - 1] += 1
        self.log(f"lobby {lobby.code}: r{r.id} session ranking (N={n}) " +
                 ", ".join(f"{m.cid}:w{m.wins}/{m.placement_score:.2f}"
                           for m in self.session_order(lobby)))
        return True

    def _back_in_sel(self, cid, msg, now):
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.round is None:
            return
        if msg.get("round") != lobby.round.id:
            return
        m = self._member(lobby, cid)
        if m:
            m.back_in_sel = True
        # round leader or owner back on song select ends the round for all;
        # a bot's back_in_sel only marks itself
        if cid in self._bots:
            return
        r = lobby.round
        if cid != r.leader and cid != lobby.owner_cid:
            return
        if lobby.phase == "RESULTS":
            self.log(f"lobby {lobby.code}: r{r.id} leader/owner "
                     f"back_in_sel -> round_end")
            self._round_end(lobby, now)
        elif lobby.phase == "PLAYING":
            r.leader_back = True

    def _round_end(self, lobby, now, rotate=True):
        r = lobby.round
        if lobby.pending_pick_mode:
            mode, assignee = lobby.pending_pick_mode
            self._apply_pick_mode(lobby, mode, assignee)
        if lobby.pending_leader and self._member(lobby, lobby.pending_leader):
            next_leader = lobby.pending_leader
        elif lobby.pick_mode == "owner":
            next_leader = lobby.owner_cid
        elif (lobby.pick_mode == "assigned" and lobby.assigned_cid in
              {m.cid for m in lobby.members}):
            next_leader = lobby.assigned_cid
        elif lobby.pick_mode == "assigned":
            next_leader = lobby.owner_cid
        elif not rotate:
            next_leader = r.leader if self._member(lobby, r.leader) else (
                lobby.members[0].cid if lobby.members else None)
        else:
            next_leader = None
            idx = next((i for i, m in enumerate(lobby.members)
                        if m.cid == r.leader), -1)
            for k in range(1, len(lobby.members) + 1):
                cand = lobby.members[(idx + k) % len(lobby.members)]
                if cand.connected:
                    next_leader = cand.cid
                    break
            if next_leader is None and lobby.members:
                next_leader = lobby.members[0].cid
        lobby.phase = "SELECT"
        lobby.group_paused = False
        lobby.vote = None
        lobby.sel_pv = None
        lobby.sel_name = None
        lobby.acks = {}
        lobby._sel_status_sent = None
        lobby.pending_leader = None
        ended = lobby.round
        lobby.round = None
        for m in lobby.members:
            m.role = "player"
            m.loaded = False
            m.final = None
            m.back_in_sel = False
            m.rdiff = None
        if next_leader:
            lobby.leader_cid = next_leader
        self.log(f"lobby {lobby.code}: r{ended.id} end -> leader={next_leader}")
        self._broadcast(lobby, {"t": "round_end", "round": ended.id,
                                "next_leader": next_leader})
        self._broadcast_lobby(lobby)

    # ------------------------------------------------- tick
    def tick(self, now):
        for lobby in list(self.lobbies.values()):
            self._tick_lobby(lobby, now)

    def _tick_lobby(self, lobby, now):
        r = lobby.round
        if r is not None and r.started and not r.start_sent \
                and r.countdown_at is not None \
                and now >= r.countdown_at:
            r.start_sent = True
            self._broadcast(lobby, {"t": "start", "round": r.id,
                                    "dropped": r.dropped})
        if lobby.vote is not None and now >= lobby.vote["ends_at"]:
            self._send_pause_vote(lobby, now, expired=True)
            lobby.vote = None
        if lobby.phase == "SELECT":
            # disconnected leader handoff at 10s
            for m in list(lobby.members):
                if m.connected or m.disconnect_at is None:
                    continue
                if now - m.disconnect_at >= SELECT_DC_REMOVED_S:
                    self.log(f"lobby {lobby.code}: {m.cid} removed (60s dc)")
                    self._remove_member(lobby, m.cid, "left", now)
                    return
            lead = next((m for m in lobby.members
                        if m.cid == lobby.leader()), None)
            if (lead is not None and not lead.connected and
                    lead.disconnect_at is not None and
                    now - lead.disconnect_at >= SELECT_LEADER_HANDOFF_S):
                idx = lobby.members.index(lead)
                for k in range(1, len(lobby.members) + 1):
                    cand = lobby.members[(idx + k) % len(lobby.members)]
                    if cand.connected:
                        lobby.leader_cid = cand.cid
                        self._event(lobby, "leader", cand.cid, cand.name)
                        self._broadcast_lobby(lobby)
                        break
            return

        r = lobby.round
        if r is None:
            return

        if lobby.phase == "LOADING":
            if r.leader_loaded_at is not None:
                if now - r.leader_loaded_at >= LEADER_LOADED_START_S:
                    not_loaded = [p for p in r.players
                                  if not (self._member(lobby, p) and
                                          self._member(lobby, p).loaded)]
                    self._do_start(lobby, now, not_loaded)
            elif now - r.launch_at >= NO_LEADER_LOAD_S:
                loaded = [p for p in r.players
                          if self._member(lobby, p) and
                          self._member(lobby, p).loaded]
                if loaded:
                    self._do_start(lobby, now,
                                   [p for p in r.players if p not in loaded])
                else:
                    self.log(f"lobby {lobby.code}: r{r.id} aborted (no loads)")
                    self._round_end(lobby, now, rotate=False)
            return

        if lobby.phase == "PLAYING":
            if lobby.board_dirty and \
                    now - lobby.last_board_at >= BOARD_MIN_INTERVAL_S:
                lobby.board_dirty = False
                lobby.last_board_at = now
                self._send_board(lobby)
            self._check_complete(lobby, now)
            if lobby.phase != "PLAYING":
                return
            if r.players and all(
                    (not self._member(lobby, p)) or
                    (not self._member(lobby, p).connected and
                     self._member(lobby, p).disconnect_at is not None and
                     now - self._member(lobby, p).disconnect_at >=
                     PLAYING_ALL_DC_ABORT_S)
                    for p in r.players):
                self.log(f"lobby {lobby.code}: r{r.id} aborted (all dc 60s)")
                self._round_end(lobby, now, rotate=False)
            return

        if lobby.phase == "RESULTS":
            all_back = all(
                (not self._member(lobby, p)) or
                self._member(lobby, p).back_in_sel or
                not self._member(lobby, p).connected
                for p in r.players)
            if all_back or (r.complete_at is not None and
                            now - r.complete_at >= ROUND_END_ALL_BACK_S):
                self._round_end(lobby, now)

    # ------------------------------------------------- admin
    def on_admin(self, cmd, now):
        c = cmd.get("cmd")
        if c == "lobbies":
            out = {}
            for code, l in self.lobbies.items():
                out[code] = {
                    "phase": l.phase,
                    "leader": l.leader(),
                    "round": l.round.id if l.round else 0,
                    "sel_pv": l.sel_pv,
                    "same_diff": l.same_diff,
                    "members": [{"cid": m.cid, "name": m.name,
                                 "connected": m.connected, "role": m.role,
                                 "loaded": m.loaded,
                                 "plat": self._client_plat.get(m.cid, "switch"),
                                 "diff": self.diff_of(l, m)[0],
                                 "ed": self.diff_of(l, m)[1],
                                 "nosong": m.cid in self.nosong(l),
                                 "module": self._client_modules.get(m.cid, -1),
                                 "autoplay": m.autoplay,
                                 "autoplay_pct": m.autoplay_pct,
                                 "autoplay_mode": m.autoplay_mode,
                                 "final": m.final is not None,
                                 "wins": m.wins,
                                 "placement_score": round(
                                     m.placement_score, 4),
                                 "places": list(m.places),
                                 **{k: m.state.get(k, 0) for k in
                                    ("score", "combo", "pct", "life",
                                     "border")}}
                                for m in l.members],
                }
            return out
        if c == "set_opt":
            lobby = self.lobbies.get(cmd.get("code"))
            if lobby is None:
                return {"error": "not_found"}
            if cmd.get("key") != "same_diff" or \
                    not isinstance(cmd.get("value"), bool):
                return {"error": "bad_opt"}
            self._set_opt(lobby.owner_cid, {"key": "same_diff",
                                            "value": cmd["value"]}, now)
            return {"ok": True}
        if c == "kick":
            cid = cmd.get("cid")
            lobby = self._lobby_of(cid)
            if lobby is None:
                return {"error": "not_in_lobby"}
            self.log(f"lobby {lobby.code}: {cid} kicked (admin)")
            self._remove_member(lobby, cid, "kicked", now)
            return {"ok": True}
        if c == "set_leader":
            code = cmd.get("code")
            cid = cmd.get("cid")
            lobby = self.lobbies.get(code)
            if lobby is None:
                return {"error": "not_found"}
            if self._member(lobby, cid) is None:
                return {"error": "not_in_lobby"}
            if lobby.phase == "SELECT":
                lobby.leader_cid = cid
                m = self._member(lobby, cid)
                self._event(lobby, "leader", cid, m.name)
                self._broadcast_lobby(lobby)
                self.log(f"lobby {code}: leader -> {cid} (admin)")
            else:
                lobby.pending_leader = cid
                self.log(f"lobby {code}: pending leader {cid}")
            return {"ok": True}
        return {"error": "unknown_cmd"}
