# Known bugs / follow-ups

## 1. Picker pressing B on song select accidentally starts a new multiplayer round (physical console)

**Status: FIXED (2026-11-16).** Confirmed cause: B on the song list leaves
pv_sel through 6 → 10 → Dest (server log `SEL ... state=10` right before
`DEST`, no 8 in between), while a real start is 8 → 10. The leader commit
now requires `prevSt == 8 && state == 10`. Verified on Ryujinx with the
exact repro (round played, back in song select on the same song, B).

**Symptom:** After a completed multiplayer round, the song picker's
console (physical Switch) is on song select. Pressing **B** to back out
to the main menu instead triggers a multiplayer round begin on **all**
consoles in the lobby.

**Repro:**

1. Play a song in an online lobby.
2. Play through one successful game to the results.
3. Return to the song select menu.
4. Do **not** scroll up or down (leave the cursor where it is).
5. Press **B** (to back out) on the picker console.
6. A multiplayer round begins on all consoles instead of exiting to
   the main menu.

**Suspected cause:** the leader commit fires on *any* entry into
pv_sel state 10:

`MpLobby.cpp` — "leader: commit on any entry into state 10"
(`prevSt != 10 && state == 10 && !s_committed` → sends
`{"t":"start_req",...}`).

`s_committed` is cleared when the round ends / the lobby returns to
SELECT, so on the next pass through song select the flag is free again.
If the B-press exit path transitions pv_sel through state 10 (scene
teardown / re-entry during the back-out animation), the edge detector
sees a fresh `prevSt != 10 && state == 10` and sends a second
`start_req` for whatever pv the cursor happens to sit on — hence the
"don't scroll" repro requirement (the cursor still sits on the
previously played song, which makes it look intentional). A fix likely
needs either gating the commit edge on the confirm input instead of
the raw state edge, or marking scene-teardown state transitions.

**Relevant logs:**

- Server: `MSG '<name>' {"t":"start_req","pv":<N>,...}` from the picker
  followed by the `{"t":"start","round":<R>,...}` broadcast — a second
  `start_req` appearing at the moment B was pressed (no new song
  selection) is the smoking gun. On the cloud host, check the Render
  logs; locally check the `mp_server.py` console/log file.
- Client: `hook_log.txt`
  (`sdmc:/MikuMikuSwitchPlugin/hook_log.txt` on Switch,
  `%APPDATA%\Ryujinx\sdcard\hook_log.txt` on Ryujinx):
  - `[MP-L] phase <a> -> <b>` — look for an unexpected
    SELECT → LOADING/LAUNCHING transition on the picker.
  - `[MP-L] launch r<N> ...` — on the followers, confirms they received
    the `start` broadcast and which pv it carried.
  - `[MP-L] banner: ...` — "Leader started the game!" / round banners.

## 2. CLEAR / NOT-CLEAR status and thresholds must be reported by the console

**Status: DONE (2026-11-16).** The game keeps the song + difficulty clear %
at `PVGameData+0x2D0D0` (70.0 on an Extreme; `+0x2D0D4` is a running border
that only reaches it at the last note). Consoles send it as `border` in
`state`/`final`, and `final` adds `clear` (computed on the final pct and
life, so passing the border mid-song and sinking under it is NOT CLEAR),
`drop` and `quit`. Server, web snapshot and web results use the console's
values; the hardcoded 80 % is gone.

The results/standings data should carry an explicit clear flag plus the
per-song clear threshold the console used — the server/web UI cannot
derive this correctly on their own.

- **Each client must report its own PASS threshold to the server.**
  The threshold varies per song *and* per difficulty, and each player
  in a round can be on a different difficulty — so every console should
  send the actual clear % it is playing against (e.g. as part of the
  round/results reporting), not leave the server guessing.
- **80 % is not a universal clear threshold.** Some songs can be
  cleared below 80 %; the threshold varies per song and per difficulty,
  so it must come from the game, not be hardcoded.
- **Falling below the threshold after passing it = NOT CLEAR.** If a
  player crosses the clear threshold mid-song but then loses life
  afterwards and finishes under it, the final status must be reported
  as NOT CLEAR. The flag has to reflect the *final* state, not a
  latched "threshold was reached at some point" state.

## 3. Notification when a player drops out early (Gets Drop X Out status)

**Status: DONE (2026-11-16).** Server event `drop_out` (once per player and
round, on life reaching 0 or a final with `drop`), routed through
`notify_style` on the consoles (popup tag "DROP OUT" / banner "X dropped
out!" / off) and shown in the web activity feed.

When someone fails a round early (loses all life results), a notification should be sent — routed through the
existing player-event notification styles, same as the combo/low life notifications.


## 4. Lobbies should remember player scores, Implement Rankings for Lobby Session

**Status: DONE (2026-11-16)** as specified below (`lobby.py
_apply_session_ranking`, applied once when a round's standings complete).
A player who leaves and rejoins keeps their record until the lobby empties.
Consoles: ROOM screen podium = session top 3, member list in session order,
grey 1/2/3 trophies with counts. Web: Members sorted by session rank with
trophies; song select shows the session top 3.

In the multiplayer lobby menu and in the web portal, players should be ordered by how many wins they have in the current lobby session. On the client and web while in song select, it should show how many times the user got 1st, 2nd, 3rd in the current lobby session. It should show an icon with a trophy with a 1 in it, then number of first places, then next to it icon with a trophy with a 2 in it, then number of second places, then next to it icon with a trophy with a 3 in it, then number of third places. 

Here's the system for lobby rankings:

Lobby Ranking Specification (Up to 8 Players)
1. State Tracked Per Player
wins (integer, starts at 0)
placementScore (float, starts at 0.0)

2. End-of-Round Scoring Rules
Guardrail: Only update wins and placementScore if totalPlayers >= 2 for that round. If totalPlayers < 2, ignore the round for ranking purposes.

For each player in a valid round (N = totalPlayers, P = 1-indexed placement from 1 to N):

Wins: If P === 1, increment player.wins += 1.

Normalized Placement Score: Add (N - P) / (N - 1) to player.placementScore.

Why: Scales placements between 1.0 (1st) and 0.0 (last) regardless of lobby size, ensuring 7th (0.14) always beats 8th (0.00) in an 8-player round.

Ties: If players tie for a placement, assign them the average of the tied positions (e.g., two players tied for 4th both use P = 4.5).

3. Dynamic Joins & Leaves
Late Joiners: Initialize with wins = 0 and placementScore = 0.0. Cumulative scoring naturally prevents a late joiner with 1 win from outranking an existing player with 1 win plus prior round placements.

Mid-Round Leavers: Include them in N for the current round and assign them last place (0.0 score) so remaining players get full credit for beating them. Remove leavers from the active leaderboard once the round ends.

4. Sorting & Display Logic
Sort all currently active players at the end of each round using a strict two-tier sort:

Primary: wins (descending)

Tie-Breaker: placementScore (descending)

Slice the top 3 (sortedPlayers.slice(0, 3)) to display 1st, 2nd, and 3rd place to the players.