#!/usr/bin/env python3
"""lobby.py - MegaMix LAN MP lobby/round engine. PURE logic, no asyncio.

Protocol v1 (NDJSON, "t" = type)
C->S: hello / ping (existing); set_name {name}; lobby_create {}; lobby_join {code};
      lobby_leave {}; sel {pv, name} (leader, SELECT only);
      sel_ack {pv, has:bool, ok:bool, fb_pv:int (-1 none), fb_name:str};
      start_req {pv, name} (leader, SELECT only); launch_fail {round};
      loaded {round}; state {round, score, combo, pct, life};
      final {round, score, pct, max_combo, jc:[5], life}; back_in_sel {round}.
S->C: welcome/error/pong (existing);
      lobby {code, phase, leader, you, round, sel_pv,
             members:[{cid,name,connected,role,loaded}]}
      (phase in SELECT/LOADING/PLAYING/RESULTS; role in player/spectator/dropped)
      lobby_left {reason: left|kicked|closed};
      lobby_error {code: not_found|already_in_lobby|not_in_lobby|not_leader|bad_phase, msg};
      sel {pv, name};
      sel_status {pv, missing:[{cid,name,fb_name|null}], cant:[{cid,name}]};
      launch {round, pv, name, players:[cid], spectators:[cid]};
      start {round, dropped:[cid]};
      board {round, e:[{cid,name,score,pct,connected}]};
      event {kind, cid, name, value};
      standings {round, complete:bool, e:[{cid,name,score,pct,max_combo,life,rank,final:bool}]};
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


class Member:
    __slots__ = ("cid", "name", "connected", "role", "loaded", "final",
                 "back_in_sel", "state", "prev_combo_bucket", "low_life_armed",
                 "disconnect_at", "last_activity",
                 "ready", "ready_reason", "ready_pv", "autoplay")

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


class Round:
    __slots__ = ("id", "pv", "name", "leader", "players", "spectators",
                 "dropped", "launch_at", "leader_loaded_at", "started",
                 "first_final_at", "complete_at", "complete_announced")

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
        self.first_final_at = None
        self.complete_at = None
        self.complete_announced = False


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
        self.group_paused = False
        self.vote = None   # {"kind","agreed":set,"ends_at":float}
        self.sel_pv = None
        self.sel_name = None
        self.acks = {}              # cid -> ack dict for current sel_pv
        self._sel_status_sent = None  # last (missing,cant) signature sent to leader
        self.seq = 0
        self.round = None
        self.pending_leader = None
        self.last_board_at = 0.0
        self.board_dirty = False

    def effective_leader(self):
        if self.pick_mode == "owner":
            return self.owner_cid
        if self.pick_mode == "assigned" and self.assigned_cid in                 {m.cid for m in self.members}:
            return self.assigned_cid
        return self.leader_cid

    def leader(self):
        return self.effective_leader()


class LobbyManager:
    def __init__(self, log=None):
        self.lobbies = {}           # code -> Lobby
        self.by_cid = {}            # cid -> Lobby
        self.outbox = []            # (cid, msg)
        self.log = log or (lambda s: None)
        self.max_members = 8        # real consoles cap at 8; raise for demos
        self._rng = random.Random()
        self._client_names = {}     # cid -> display name (all clients)

    # ------------------------------------------------- helpers
    def _lobby_of(self, cid):
        return self.by_cid.get(cid)

    def _member(self, lobby, cid):
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

    def _lobby_msg(self, lobby, you_cid):
        return {"t": "lobby", "code": lobby.code, "phase": lobby.phase,
                "leader": lobby.leader(), "you": you_cid,
                "owner": lobby.owner_cid, "pick_mode": lobby.pick_mode,
                "assigned": lobby.assigned_cid,
                "group_paused": lobby.group_paused,
                "round": lobby.round.id if lobby.round else 0,
                "sel_pv": lobby.sel_pv,
                "members": [{"cid": m.cid, "name": m.name,
                             "connected": m.connected, "role": m.role,
                             "loaded": m.loaded,
                             "ready": m.ready, "reason": m.ready_reason,
                             "autoplay": m.autoplay}
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
    def on_hello(self, cid, name, mod, now):
        self._client_names[cid] = name
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
            if r.started:
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
        elif t == "ready":
            self._ready(cid, msg, now)
        elif t == "set_pick_mode":
            self._set_pick_mode(cid, msg, now)
        elif t == "autoplay":
            m = self._member(self._lobby_of(cid), cid)
            if m:
                m.autoplay = bool(msg.get("on"))
                lobby = self._lobby_of(cid)
                if lobby:
                    self._broadcast_lobby(lobby)
        elif t in ("pause_req", "resume_req"):
            self._pause_req(cid, msg, now, t == "pause_req")

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
        m.role = "player" if lobby.phase == "SELECT" else "spectator"
        lobby.members.append(m)
        self.by_cid[cid] = lobby
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

    def _remove_member(self, lobby, cid, reason, now):
        idx = next((i for i, m in enumerate(lobby.members) if m.cid == cid), -1)
        if idx < 0:
            return
        was_leader = lobby.leader() == cid
        m = lobby.members.pop(idx)
        self.by_cid.pop(cid, None)
        self._send(cid, {"t": "lobby_left", "reason": reason})
        if not lobby.members:
            del self.lobbies[lobby.code]
            self.log(f"lobby {lobby.code} closed (empty)")
            return
        if lobby.owner_cid == cid:
            lobby.owner_cid = lobby.members[0].cid
            self._event(lobby, "leader", lobby.owner_cid,
                        lobby.members[0].name)
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

    def _pause_req(self, cid, msg, now, pause):
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.round is None:
            return
        if msg.get("round") != lobby.round.id:
            return
        if cid not in lobby.round.players:
            return
        kind = "pause" if pause else "resume"
        if lobby.owner_cid == cid:
            # owner: immediate
            lobby.vote = None
            lobby.group_paused = pause
            self._broadcast(lobby, {"t": kind, "by": cid})
            self.log(f"lobby {lobby.code}: {kind} by owner {cid}")
            return
        # vote: single open vote; a req while open counts as agreeing
        v = lobby.vote
        if v is None or v["kind"] != kind or now >= v["ends_at"]:
            v = {"kind": kind, "agreed": {cid}, "ends_at": now + PAUSE_VOTE_S}
            lobby.vote = v
        else:
            v["agreed"].add(cid)
        self._send_pause_vote(lobby, now)
        need = {c for c in lobby.round.players
                if self._member(lobby, c) is not None and
                self._member(lobby, c).connected}
        if v["agreed"] >= need and len(need) > 0:
            lobby.vote = None
            lobby.group_paused = pause
            self._broadcast(lobby, {"t": kind, "by": cid})
            self.log(f"lobby {lobby.code}: {kind} vote passed")

    def _send_pause_vote(self, lobby, now, expired=False):
        v = lobby.vote
        if v is None:
            return
        need = sum(1 for c in lobby.round.players
                   if self._member(lobby, c) is not None and
                   self._member(lobby, c).connected)
        msg = {"t": "pause_vote", "kind": v["kind"],
               "agreed": sorted(v["agreed"]), "need": need,
               "ends_in": max(0, int(v["ends_at"] - now))}
        if expired:
            msg["expired"] = True
        self._broadcast(lobby, msg)

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
        m.state = {"score": score, "combo": combo, "pct": pct, "life": life}
        if combo >= 50 and combo // 50 > prev_bucket:
            m.prev_combo_bucket = combo // 50
            self._event(lobby, "combo", cid, m.name, combo)
        if life <= 40 and prev_life > 40 and m.low_life_armed:
            m.low_life_armed = False
            self._event(lobby, "low_life", cid, m.name, life)
        elif life > 60:
            m.low_life_armed = True
        lobby.board_dirty = True

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
                      "connected": m.connected})
        e.sort(key=lambda x: (-x["pct"], -x["score"]))
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
        m.final = {"score": msg.get("score", 0), "pct": msg.get("pct", 0.0),
                   "max_combo": msg.get("max_combo", 0),
                   "jc": msg.get("jc", []), "life": msg.get("life", 0)}
        m.last_activity = now
        m.state["score"] = m.final["score"]
        m.state["pct"] = m.final["pct"]
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
                     m.connected)
            rows.append({"cid": m.cid, "name": m.name,
                         "score": f["score"] if f else m.state.get("score", 0),
                         "pct": f["pct"] if f else m.state.get("pct", 0.0),
                         "max_combo": f["max_combo"] if f else 0,
                         "life": f["life"] if f else 0,
                         "final": f is not None or not m.connected or stale,
                         "connected": m.connected})
        rows.sort(key=lambda x: (-x["pct"], -x["score"]))
        for i, row in enumerate(rows):
            row["rank"] = i + 1
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
            self._send_standings(lobby, now)

    def _back_in_sel(self, cid, msg, now):
        lobby0 = self._lobby_of(cid)
        if (lobby0 is not None and lobby0.owner_cid == cid and
                lobby0.phase == "RESULTS"):
            self.log(f"lobby {lobby0.code}: owner back_in_sel -> round_end")
            self._round_end(lobby0, now)
            return
        lobby = self._lobby_of(cid)
        if lobby is None or lobby.round is None:
            return
        if msg.get("round") != lobby.round.id:
            return
        m = self._member(lobby, cid)
        if m:
            m.back_in_sel = True

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
                    "members": [{"cid": m.cid, "name": m.name,
                                 "connected": m.connected, "role": m.role,
                                 "loaded": m.loaded}
                                for m in l.members],
                }
            return out
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
