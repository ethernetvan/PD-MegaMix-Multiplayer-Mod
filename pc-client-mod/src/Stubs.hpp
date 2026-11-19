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

/* module db: report-only stub (avatar::Resolve always falls to silhouette) */
namespace moduledb {
struct Info { int32_t id; int32_t chara; int32_t sort_index; std::string name; };
bool Available();
void MarkReady();
const Info* Find(int32_t id);
int32_t DefaultModuleFor(int32_t chara);
void DbgDump();
} // namespace moduledb

/* Song-select status hints (Switch pill). H2 — stubbed. */
namespace Hints {
void ProcessInput(uint64_t tapped, uint64_t down);
void SetPvSelActive(bool active);
void SetSongConfirmActive(bool active);
} // namespace Hints
