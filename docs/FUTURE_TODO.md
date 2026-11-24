# Future TODOs (not scheduled)

Longer-term multiplayer ideas. Current work lives in `docs/NEXT_SESSION_TODO.md` and
`docs/MULTIPLAYER_STATUS.md`.

## Song-settings sync
The server syncs the round's song settings so every console plays under the same rules:
- No Fail mode on for everyone (or off for everyone). DONE 2026-11-24: room option `no_fail`
  (`docs/MULTIPLAYER_STATUS.md` Round 18).
- Modifiers (e.g. Hi-Speed / Hidden / Sudden and similar) applied to every console when the picker
  changes them.
- A room setting (owner, Room Config) to enable/disable this sync; when disabled, each console keeps
  its own settings.

## Results screen: full player breakdown after the first tap (user, 2026-11-02)
- In a multiplayer room, the first tap/press on the results screen dismisses the hexagon column and opens a
  full-screen (or most-of-screen) scrollable panel with every player's score distribution, like the web
  frontend's results cards: COOL / FINE / SAFE / SAD / MISS counts with a per-player judgment bar, score, %,
  max combo, and the outcome (CLEAR / NOT CLEAR / DROP×OUT, failed early at life 0, quit the round, dropped).
- Needs the per-player judgment counts on the console (the server already receives `jc` in each `final`; it
  would have to forward them in `standings`), and a new page in the blind results-menu model (the tap that
  opens the panel must not count as the game's A).
