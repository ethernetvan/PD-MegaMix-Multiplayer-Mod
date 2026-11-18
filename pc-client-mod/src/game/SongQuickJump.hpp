#pragma once
// Same API as the Switch overlays/SongQuickJump.hpp; PC implementation lives
// in game/MpSelPc.cpp over the real selector structs.
#include <cstdint>
#include <cstddef>

namespace SongQuickJump {
    void Draw();                                   /* no-op on PC (H1) */
    void ParsePvDbBuffer(const char* data, size_t length); /* no-op */
    void SetPvSelActive(bool active, void* sel = nullptr);
    bool IsVisible();                              /* false on PC */

    bool JumpSelectorToPv(void* sel, int32_t pvId, bool keepDifficulty);
    bool JumpSelectorToPvAt(void* sel, int32_t pvId, int32_t diff, int32_t ed);
    bool PvSelectableAt(const void* sel, int32_t pv, int32_t diff, int32_t ed);
    int32_t SelVisibleCount(const void* sel, int32_t diff, int32_t ed);
    bool ClosestSelectable(const void* sel, int32_t pv, int32_t curDiff,
                           int32_t curEd, int32_t* outDiff, int32_t* outEd);
    bool GetSongDbEntry(int i, int32_t* id, char* name, size_t nameCap);
    int32_t FindPvByName(const char* name);
    bool    PvInDb(int32_t pv);
    bool    PvName(int32_t pv, char* name, size_t nameCap);
}
