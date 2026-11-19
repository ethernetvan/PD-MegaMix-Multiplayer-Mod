#pragma once
// PC port: stubs for Switch-side helpers that have no H1 equivalent on PC.
// moduledb returns defaults; the virtual keyboard and Hints UI are not
// ported yet. AutoPlay is a real module now (game/MpAutoPlay.hpp) — it is
// included here so the stub-era include sites keep working.
#include <cstdint>
#include <cstddef>
#include <string>
#include "game/MpAutoPlay.hpp"

namespace CustomizeSelUi {
bool IsInCustomizeScene();
bool IsOpen();
} // namespace CustomizeSelUi

/* module db: the game's merged module table, read straight out of memory
 * (MpModuleDb.cpp — PC port of switch-client-mod MpModuleDb.cpp). */
namespace moduledb {
struct Info { int32_t id, chara, sortIndex, cos; uint32_t sig; };
bool Available();
void MarkReady();
/* false: no such module on this console, or the table isn't loaded yet.
 * sig = FNV-1a 32 over chara, cos, sort_index (LE int32 each), never 0:
 * language-independent, and differs when a mod reuses an official id. */
bool Find(int32_t module, Info* out);
/* chara 0..9 -> that character's default module; -1 for anything else */
int32_t DefaultModuleFor(int32_t chara);
void DbgDump();
/* internal (MpAvatar.cpp): the raw table entry for sprite-set/spr ids */
const uint8_t* RawEntry(int32_t id);
} // namespace moduledb

/* Song-select status hints (Switch pill). H2 — stubbed. */
namespace Hints {
void ProcessInput(uint64_t tapped, uint64_t down);
void SetPvSelActive(bool active);
void SetSongConfirmActive(bool active);
} // namespace Hints
