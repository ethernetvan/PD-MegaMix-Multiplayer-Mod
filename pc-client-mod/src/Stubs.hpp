#pragma once
// PC port: stubs for Switch-side helpers that have no H1 equivalent on PC.
// AutoPlay is report-only (g_enabled stays false); moduledb returns defaults;
// the virtual keyboard and Hints UI are not ported yet.
#include <cstdint>
#include <cstddef>
#include <string>

namespace AutoPlay {
enum Mode : int { Mode_Off = 0, Mode_Safe = 1, Mode_SomeSad = 2,
                  Mode_AllSad = 3, Mode_Custom = 4, Mode_Count = 5 };
extern bool g_enabled;     /* always false on PC (report-only) */
extern int  g_hitPct;
extern int  g_mode;
void init();
void OnGameTaskFrame();
void Update(float dt);
void PostUpdate(float dt);
void IndicatorVisible(bool v);
bool IndicatorVisible();
void DrawIndicator(float x, float y, float s);
void ProcessSlideHit(int hitType);
int  ModeFromName(const char* name);
const char* ModeName(int mode = -1);
const char* ModeTag(int mode);
int  EffectivePct();
} // namespace AutoPlay

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
