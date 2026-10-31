# Avatar poses — long-term plan

Status: plan only (not implemented). Today each lobby member's avatar shows the vocaloid of the first
module in their **Module Shared Set A** inside a pink/orange hexagon (see `docs/MULTIPLAYER_STATUS.md`).
This document describes how players could later pick one of a few **default poses** for that vocaloid and
have everyone in the room see it.

## Goals
- Pick a pose from a short, curated list per vocaloid (e.g. *Idle*, *Peace*, *Wave*, *Victory*, *Cheer*).
- No extra mandatory screens: the default stays the current portrait; choosing a pose is optional.
- Works with touch and with controller buttons (same focus model as the rest of the overlay).
- Everyone in the room sees the chosen pose in the outfit (module) the player actually has equipped.
- Cheap at runtime: no hitches in song select, bounded memory, nothing loaded while a song is playing.

## Non-goals
- Free-form posing / bone editing (the debug Motion Player already covers that for screenshots).
- Custom uploaded images (moderation, size limits and transport are out of scope).

## Data model
- **Pose catalog** (`sdmc:/MikuMikuSwitchPlugin/avatar_poses.toml`, shipped with the mod, editable):
  ```toml
  [[pose]]
  id = "peace"            # stable string id sent over the network
  name = "Peace"          # UI label
  motion = "CMN_MRK_..."  # mot_db motion name (resolved to an id at boot)
  frame = 42              # frame to freeze on
  charas = ["MIK","RIN","LEN","LUK","MEI","KAI"]   # omit = all
  camera = "bust"         # framing preset: bust | full
  ```
  Curate 4–6 poses that exist for every base character (customize-screen idles, result/rank motions).
- **Player choice** in `config.toml` `[multiplayer] avatar_pose = "peace"` (empty = portrait, the current
  behaviour).
- **Protocol**: extend the existing avatar report `avatar {chara, module}` with `pose` (string id, optional).
  The server stores it per member and includes it in the `lobby` member list. Old clients ignore the field;
  missing `pose` = portrait. Unit tests in `server/test_lobby.py`.

## Rendering options (decide after RE)
1. **Render-to-texture with the game's own character renderer** (preferred if feasible).
   Spawn a hidden `rob_chara` with the member's module while song select is idle, apply the catalog motion
   at the frozen frame, render one frame into an off-screen NVN colour target (bust or full framing), copy
   the result into an RGBA texture registered in the ImGui backend's texture pool, then free the character.
   Each (module, pose) result is cached in RAM and on SD (`avatars/cache/<module>_<pose>.rgba`, ~256×256),
   so it is rendered at most once per console.
   Needs RE: rob_chara create/destroy, motion set loading + freeze, an off-screen render pass, and the memory
   budget in song select (the customize screen proves the game can load a model there).
2. **Pre-rendered pose thumbnails produced on PC** (fallback). A dev tool drives Ryujinx through the in-game
   customize/gallery viewer, captures each (character, pose) and writes RGBA files that the deploy step copies
   to the SD card. Simpler and robust, but limited to default outfits and to assets kept outside git.
3. **2D game sprites** if the romfs turns out to contain per-character pose illustrations (check result/rank
   and Mix Mode sprite sets) — cheapest of all if they exist.

Every path falls back to the current module-thumbnail portrait, then to the drawn silhouette.

## UI
- Home screen: the name area becomes **Edit Profile** (name + avatar) or a fourth small "Avatar" button;
  Room screen: tapping/selecting your own podium avatar opens the same screen.
- Avatar screen: large hexagon preview on the left, pose grid on the right (portrait first), A/tap to apply,
  B to cancel. Previews render lazily (spinner hexagon while a pose renders) and only for the visible poses.
- Changing the equipped module still updates the avatar automatically; the chosen pose is kept per vocaloid
  (fall back to portrait when the new vocaloid lacks that pose).

## Phases
1. Protocol + config + server field (`pose`), UI stub that only offers *Portrait* — ship-safe.
2. RE spike: decide the rendering option (render-to-texture prototype on Ryujinx and on hardware, memory
   numbers in song select, time per render).
3. Pose catalog: pick motions/frames per character, verify framing for all base vocaloids.
4. Renderer + cache (SD + RAM, LRU of ~16 textures, never during LOADING/PLAYING).
5. Avatar screen UI (touch + pad), live previews, persistence.
6. Hardening: custom/mod modules other consoles lack (fallback to that chara's default module), missing
   motions, SD write failures, hot module changes while the overlay is open.

## Risks / open questions
- Off-screen rendering through the game's pipeline may conflict with its own frame graph; prototype first.
- Memory: loading an extra character in song select could exceed the budget on hardware (measure).
- Some modules use per-module motions/props; verify catalog poses look right with accessories.
- Network: pose ids must be validated server-side against a known list to avoid arbitrary strings.
