#pragma once
#include <cstdint>
#include <cstddef>

namespace SongQuickJump {
    void Draw();
    void ParsePvDbBuffer(const char* data, size_t length);
    void SetPvSelActive(bool active, void* sel = nullptr);
    bool IsVisible();

    /* Jump a PVSelectorSwitch to pvId. keepDifficulty preserves the
     * selector's current difficulty/edition (MP); false = original
     * (difficulty 3 / edition 0 + sortfilter SFX). */
    bool JumpSelectorToPv(void* sel, int32_t pvId, bool keepDifficulty);

    /* Jump to pvId at an explicit difficulty/edition (no SFX). */
    bool JumpSelectorToPvAt(void* sel, int32_t pvId, int32_t diff, int32_t ed);
    /* pv present in sorted_pv_lists[8*sort + 2*diff + ed] of the
     * selector's current sort mode. */
    bool PvSelectableAt(const void* sel, int32_t pv, int32_t diff, int32_t ed);
    /* Nearest (diff, ed) where pv is selectable; same difficulty + other
     * edition first, then curDiff±k with edition 0 before 1. */
    bool ClosestSelectable(const void* sel, int32_t pv, int32_t curDiff,
                           int32_t curEd, int32_t* outDiff, int32_t* outEd);

    /* Song db accessor for list_pvs; false past end. name <= 24 chars. */
    bool GetSongDbEntry(int i, int32_t* id, char* name, size_t nameCap);

    /* Lookup helpers (MP lobby). */
    int32_t FindPvByName(const char* name);      /* name match, no id toggle */
    bool    PvInDb(int32_t pv);
    bool    PvName(int32_t pv, char* name, size_t nameCap);  /* en else jp */
}
