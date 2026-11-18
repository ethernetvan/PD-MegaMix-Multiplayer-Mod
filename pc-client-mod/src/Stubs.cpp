// PC port stubs — see Stubs.hpp.
#include "Stubs.hpp"
#include "platform/MpLog.hpp"
#include <cstring>

namespace AutoPlay {
bool g_enabled = false;
int  g_hitPct = 100;
int  g_mode = Mode_Off;
void init() {}
void OnGameTaskFrame() {}
void Update(float) {}
void PostUpdate(float) {}
void IndicatorVisible(bool) {}
bool IndicatorVisible() { return false; }
void DrawIndicator(float, float, float) {}
void ProcessSlideHit(int) {}
int ModeFromName(const char* name) {
    if (!name) return Mode_Off;
    if (!strcmp(name, "safe"))    return Mode_Safe;
    if (!strcmp(name, "somesad")) return Mode_SomeSad;
    if (!strcmp(name, "allsad"))  return Mode_AllSad;
    if (!strcmp(name, "custom"))  return Mode_Custom;
    return Mode_Off;
}
const char* ModeName(int m) {
    if (m < 0) m = g_mode;
    switch (m) {
    case Mode_Safe:    return "safe";
    case Mode_SomeSad: return "somesad";
    case Mode_AllSad:  return "allsad";
    case Mode_Custom:  return "custom";
    default:           return "off";
    }
}
const char* ModeTag(int m) { return ModeName(m); }
int EffectivePct() { return g_hitPct; }
} // namespace AutoPlay

namespace moduledb {
bool Available() { return false; }
void MarkReady() {}
const Info* Find(int32_t) { return nullptr; }
int32_t DefaultModuleFor(int32_t) { return -1; }
void DbgDump() { WriteLog("[MP] moduledb: stub (no module table on PC)\n"); }
} // namespace moduledb

namespace Hints {
void ProcessInput(uint64_t, uint64_t) {}
void SetPvSelActive(bool) {}
void SetSongConfirmActive(bool) {}
} // namespace Hints
