# Next session: handoff (written 2026-11-02 ~08:55, end of session 3)

Read `AGENTS.md` and `docs/MULTIPLAYER_STATUS.md` (Round 8) first. Older handoff versions (with the full bug
write-ups 1.1-1.21 from session 2) are archived at `mm-re/tmp/NEXT_SESSION_TODO.session2.md`.
Future ideas (not scheduled): `docs/FUTURE_TODO.md`.

User rules for this work: commit as you go (Evan Thomas identity, NO Devin attribution anywhere). Test every
change on the LATEST build in Ryujinx with **three emulators** (the user asked for 3) plus bots when you need
more players; use the real Switch for DLC / fan-import cases and real-controller input. Add every newly found
bug to this file.

## RESUME HERE

### Commits in session 3 (on top of `f9f16c6`), all verified on emulators unless noted
- `815f20b` blind results/pause menu model (`mp/MpMenu.cpp`): results auto-return when the leader/owner leaves,
  Retry swallowed in rooms with 2+ members (results + pause; Lag Config/Options too), retry recovery, group-pause
  reconciler, real-input edges combined across Npad styles, quit-from-pause sends the final; server: pause votes
  ignore finished players.
- `79e244c` live boards show % + life bar; MP Live moved top-right below the note-rating icons.
- `ce0a059` song-select following uses the selector's own lists (missing songs, closest difficulty, max 3 jump
  attempts, "Not available for:" pill for the picker).
- `108cdc2` server admin-socket crash fix. `0c1e636` input edges keyed by Npad style x controller id.
- `441132d` bot.py: pause keeps the song clock, `--vote delay|never|instant`, `start` uses the pick, `scene`.
- `514a519` docs.
- `9a9afea` tied results = one hexagon cluster (pair / triangle); partial AutoPlay cheat (ZR x3 cycles
  100/75/50/25 %, badge shows AUTO N%; `admin.py autoplay on|off --pct N`, deterministic per note so equal % on
  two consoles ties); availability = what the selector actually shows (NC style + All-folder filter).
- `cae89dc` ties share the LOWEST position everywhere (two tied for first are both 2nd; 1, 3, 3 ...); tied names
  stacked one per line, local player first; 4+ tied = three hexes with a "+" and "and X others"; "Waiting for
  players" popup for every member; in-song banners small at top-left; followers can use the module picker /
  customize (follower lock only on the song list); `admin.py lobby_create|lobby_join CODE|lobby_leave`.
- `7836bb3` MP works with `new_classics = false` (MpSceneHooks; verified on ryu3); server version warning compares
  against the other connected consoles; `admin.py menu_holds ...` debug knob (no tuning run yet).

### Still open, in order
1. **Physical Switch: DONE at the end of session 3.** `config.toml.final` (server `10.169.0.37`, ports
   7777/7778) is back on the console (read back identical) and the `7836bb3` build is deployed (`subsdk9` md5
   `7f9d2b50c7059e39e4e55534eb520f18`, same as the main Ryujinx install). The test server and cut-proxy are
   stopped. (`config.toml.final` was overwritten by mistake in session 3 and rebuilt from `config.toml.pre_s5`
   with the ports set to 7777/7778; the test copy pointing at 37777 is `config.toml.s5test`.)
2. **Pause on real hardware (needs a human hand):** sys-botbase `click PLUS/MINUS/HOME` never reach the game, so
   none of these were checked on the console: one physical Plus press -> exactly one `pause_req`; owner Plus
   pauses/resumes everyone; A on Resume and B send `resume_req`; Retry / Lag Config / Options swallowed; Up + A
   leaves the round with a `final`. All verified on Ryujinx (real key = VK_OEM_PLUS via PostMessage).
3. **Switch as leader sat on the confirm panel (state 8)** once with the server alive (botbase A clicks); most
   likely botbase input dropping. Re-check with a physical press. (The other time the test server was dead.)
4. **Results auto-return timings (TODO 6):** `admin.py menu_holds --init 1200 --score_a 900 --vp 1200 --menu 600
   --nav 250` on all 3 emulators, rounds with `autoplay on --pct 0` (fast fails); need 6 consecutive follower
   auto-returns without `exit_timeout` / `retry_detected` in the dbg_menu lines, then make the passing values
   (+100 ms) the defaults in `MpMenu.cpp`. Today: ~4-6 s from round_end to song select.
5. **Ryujinx boot/attract freeze (TODO 7), now frequent:** ryu1 froze on the SEGA logo / intro 3 times on
   2026-11-02 (`mm-re/tmp/freeze_0639_ryu1`, `freeze_0742_ryu1` (status bar 0.00 FPS: no frames presented while
   the mod's net thread kept running), `freeze_0802_ryu1`), plus possible freezes of ryu1/ryu3 at the end of the
   session (`freeze_0845_ryu1`, `freeze_0845_ryu3`, `multi_freeze_1/2/3`; some were only slow first connections).
   ryu1 is the only instance with audio (ryu_multi mutes 2+) and the first freeze's last guest log line was a
   `ServiceMm SetAndWait` stub loop -> hypothesis: audio/multimedia service stall in Ryujinx, not the mod.
   Planned test (not run): enable Ryujinx file logging for the ryu_multi instances (they currently write no
   log), boot ryu1 muted ~8x vs unmuted ~8x, and ~6x with `[multiplayer] enabled = false`; count freezes (no pv_sel
   INIT within 3 min). Also seen once: ryu1's process exited right after connecting (07:43:48).
   Session 4 (2026-11-02 ~13:45-14:02): ryu1 (unmuted) froze on the intro twice more and ryu3 (MUTED) froze
   too (`freeze_1349_ryu1`, `freeze_1349_ryu3`, `freeze_1359_ryu1`); ryu1 needed 3 boots. ryu2 (muted) never
   froze. A muted instance freezing weakens the audio-only hypothesis; ryu2 surviving every launch is the
   odd one out (same build, same game files, different data root).
   Later in session 4 ryu2 froze too (`freeze_1513_ryu2`: 0.00 FPS on the opening movie, Meiko in London) and
   ryu3 again (`freeze_1513_ryu3`). Every freeze so far is in the SEGA logo / opening movie, which the mod's
   `TaskAdvLogo` hook skips when `[gameplay] skipAdv = true`. The three TEST roots now run with
   `skipAdv = true` (main install untouched, still false); no boot freeze since the switch (small sample, keep
   counting). If it holds, recommend `skipAdv = true` to the user for Ryujinx, and the root cause is the
   movie playback path in Ryujinx (frames stop while the mod's net thread keeps running), not MP code.
6. **Not yet checked on the console** (all verified on emulators): connection watchdog (30 s local leave / quick
   reconnect keeps the room), ties layout, partial AutoPlay, waiting popup, in-song banners, module picker fix.
7. `tools/record_demo.py`: now runs Ryujinx WINDOWED on the second monitor (`361fa3b`: Config.json
   window_startup, game-area crop, gdigrab fallback because DISPLAY6 isn't a ddagrab output here; a 3 s gdigrab
   test of that monitor captured game frames at ~58 fps). The full tour has still never run end to end (needs the
   main install free and RAM: close the ryu_multi instances first, use `--port-base 47777` if a test server is up).
8. FT UI (`ft_ui` / `force_ft_ui`) has no MP hooks at all (PS4-style selector/results); MP needs the Switch UI.
9. Lag Config / Options stay swallowed in guarded rooms: probed, entering/leaving them gives no signal (no DBG
   field, no AET layer call), so the blind count can't follow them.
10. **DONE in `6117201`: real profile pictures on the web** (see `MULTIPLAYER_STATUS.md` Round 9). Verified
    on 3 emulators: AVATAR_IMG for modules 0/363/91, upright PNGs, lobby page with portraits
    (`mm-re/screens/web_lobby_vlwf.png`). Not checked on the real Switch.
11. **DONE in `6117201`: AutoPlay percentage** (web badge `AUTO 50%`; gold in-song pill under the life gauge
    with banners stacked below, `mm-re/screens/r1_song_banner.png`; song-select pill under the Multiplayer
    badge with the "picking a song" line below it, `r1_sel_pill.png`; no pill on results). The user's "O x4
    didn't trigger AUTO" was by design (4 taps = nothing; 3 = partial AutoPlay, 5 = toggle), but the old
    badge text also overlapped the "Multiplayer" title and was unreadable; fixed.
12. **Other players on the bottom percentage gauge (user request, session 4).** Show every other player's
    current % as a bar on/at the game's bottom gauge, same styling as the game's own fill but in per-player
    colours. The local player's own bar keeps its colour and is never covered. Measured geometry (720p):
    inner track x 130..1230 = 0..100 % (11 px per %), y 691..711. The clear border is the red line (EASY 30 %
    at x 461). Data: the live `board` (pct per member, ~state rate); interpolate between updates.
13. **Avatar upload hardening (user request, session 4).** The server must normalize every upload to the
    expected resolution, name it itself (never from the uploader), and keep nothing unsafe: square
    32..96 px only, size checked before decoding, re-encoded server-side to a fixed 96x96 PNG with only
    IHDR/IDAT/IEND, transparent pixels' RGB zeroed, garbage (blank/flat) rejected, uploads only for the
    client's own announced module, rate-limited, safe response headers (nosniff, CSP sandbox, CORP).
14. **Rank-change glow (user request, session 4).** In song: when the local player moves UP a place on the live
    board, a subtle green glow comes in from the left/right screen edges for a moment; when they drop a place,
    the same in purple. Subtle and short (about 1 s fade). Ignore the first seconds of a song and rank jitter
    (debounce plus cooldown), so it only fires on real overtakes.

### Environment at the end of session 3
- Test server **s13** (detached, ports 37777/37778/37790/38080, `mm-re/tmp/s13_server.log`, `live_server.log`
  hardlinked). `mm-re/tmp/cutproxy.py` (PID 3472) forwards :37787 -> :37777; ryu2's instance config points at
  37787 so its connection can be cut (`cut` / `restore` in `mm-re/tmp/cutproxy_ctl.txt`).
- Three emulators via ryu_multi (ryu1/ryu2/ryu3, cids a00000000000000N). **`ryu_multi.py up` does NOT refresh an
  instance's mods**: after `deploy-ryu`, copy `switch-client-mod/deploy/subsdk9` + `main.npdm` into
  `mm-re/ryu_multi/ryuN/mods/contents/01001CC00FA1A000/mods/exefs/` before restarting. `setup` rewrites every
  instance's config (re-apply ryu2's 37787 port afterwards).
- Test scripts (outside the repo): `mm-re/tmp/b6b_test.py` (room + 2 bots + forced 5-way tie + screenshots),
  `mm-re/tmp/bot_smoke.py` (bot pause clock / vote delay), `mm-re/tmp/web_tie_shot.py` (web tie card),
  `mm-re/tmp/cutproxy.py`. Probe notes: `mm-re/tmp/probe_notes.md` sessions 3-4.
- The ryu_multi windows tile on the second monitor; capture with `ImageGrab.grab(all_screens=True)` + crop
  (ryu1 (1920,0,2880,576), ryu2 (2880,0,3840,576), ryu3 (1920,576,2880,1152)).
