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

    /* Song db accessor for list_pvs; false past end. name <= 24 chars. */
    bool GetSongDbEntry(int i, int32_t* id, char* name, size_t nameCap);

    /* Lookup helpers (MP lobby). */
    int32_t FindPvByName(const char* name);      /* name match, no id toggle */
    bool    PvInDb(int32_t pv);
    bool    PvName(int32_t pv, char* name, size_t nameCap);  /* en else jp */
}
