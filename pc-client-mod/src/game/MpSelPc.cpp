// PC port: all game hooks + the selector shadow + SongQuickJump impl.
//
// The Switch mp code reads the PVSelectorSwitch struct by raw offsets
// (state @0x68, busy @0x2587C, index @0x25960, sorted_pv_lists @0x25DD0,
// pv_id @0x2617C, sort_mode @0x26198, diff @0x261A0, ed @0x261A4). The two PC
// selectors have different layouts, so each selector's Ctrl hook copies its
// live fields into a shared shadow buffer in Switch layout and mp sees that.
// Jumps/selectability go through the REAL selector via the offset tables
// below (the lobby passes the shadow pointer; we resolve to the live one).

#include "../platform/MpLog.hpp"
#include "../mp/MpNet.hpp"
#include "../mp/MpMenu.hpp"
#include "../mp/MpInput.hpp"
#include "diva.h"
#include "hooks.h"
#include "addresses.h"
#include "MpAutoPlay.hpp"
#include <cstring>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>

/* ------------------------------------------------------------------ */
/* PC selector layouts (nc pv_sel_switch.cpp / pv_sel_ps4.cpp offsets)  */
/* ------------------------------------------------------------------ */
namespace selmap {

struct Map {
    size_t state;
    size_t is_ranking_mode;
    size_t sel_pv_list;      /* SelPvList: visible-list wrapper */
    size_t sorted_pv_lists;  /* prj::vector<T>[32] */
    size_t cur_pv_list;
    size_t random_pv;
    size_t song_counts;
    size_t all_sort_index;
    size_t cur_sort_index;   /* int32_t* -> current folder's song index */
    size_t pv_id;
    size_t sort_indices;     /* int32_t[4], MM only (0 = unknown) */
    size_t sort_change_dir;  /* int32_t, MM only */
    size_t sel_disp_pv;      /* int32_t pv shown on the selected row's panel;
                              * lags pv_id while the list animates (MM only) */
    size_t index;            /* int32_t cursor index in cur_pv_list (MM only) */
    size_t sort_mode;
    size_t difficulty;
    size_t edition;
    size_t elem_size;
    bool   is_ft;
};

/* PVSelectorSwitch (MegaMix UI) */
constexpr Map MM = {
    /* state            */ 0x6C,
    /* is_ranking_mode  */ 0x2592B,
    /* sel_pv_list      */ 0x25930,
    /* sorted_pv_lists  */ 0x25EE8,
    /* cur_pv_list      */ 0x261E8,
    /* random_pv        */ 0x261F8,
    /* song_counts      */ 0x26248,
    /* all_sort_index   */ 0x2629C,
    /* cur_sort_index   */ 0x26240,
    /* pv_id            */ 0x262A4,
    /* sort_indices     */ 0x262AC,
    /* sort_change_dir  */ 0x262BC,
    /* sel_disp_pv      */ 0x258DC,
    /* index            */ 0x261F0,
    /* sort_mode        */ 0x262C0,
    /* difficulty       */ 0x262C8,
    /* edition          */ 0x262CC,
    /* elem_size        */ 0x48,
    /* is_ft            */ false,
};

/* PVselPS4 (Future Tone UI) */
constexpr Map FT = {
    /* state            */ 0x6C,
    /* is_ranking_mode  */ 0,       /* not present in nc layout */
    /* sel_pv_list      */ 0x374E8,
    /* sorted_pv_lists  */ 0x37608,
    /* cur_pv_list      */ 0x37908,
    /* random_pv        */ 0x37928,
    /* song_counts      */ 0x37978,
    /* all_sort_index   */ 0x379CC,
    /* cur_sort_index   */ 0x379D0,
    /* pv_id            */ 0x379DC,
    /* sort_indices     */ 0,       /* unknown for FT */
    /* sort_change_dir  */ 0,       /* unknown for FT */
    /* sel_disp_pv      */ 0,       /* unknown for FT */
    /* index            */ 0,       /* unknown for FT */
    /* sort_mode        */ 0x379E0,
    /* difficulty       */ 0x379F8,
    /* edition          */ 0x379FC,
    /* elem_size        */ 0x50,
    /* is_ft            */ true,
};

} // namespace selmap

/* ------------------------------------------------------------------ */
/* Shadow (Switch PVSelectorSwitch offsets the mp code reads)           */
/* ------------------------------------------------------------------ */
namespace sh {
constexpr size_t STATE      = 0x68;
constexpr size_t BUSY       = 0x2587C;
constexpr size_t INDEX      = 0x25960;
constexpr size_t LISTS      = 0x25DD0;   /* libcxx_vector_base<pvsel::PvData>[32] */
constexpr size_t PV_ID      = 0x2617C;
constexpr size_t SORT_MODE  = 0x26198;
constexpr size_t DIFF       = 0x261A0;
constexpr size_t EDITION    = 0x261A4;
constexpr size_t BUF_SIZE   = 0x27000;
} // namespace sh

static uint8_t s_shadow[sh::BUF_SIZE];

/* Currently-live selector + which UI is active. Set by the Ctrl hooks. */
static void* s_realSel = nullptr;
static const selmap::Map* s_map = nullptr;

/* pc-side adapter handle used by SongQuickJump below */
static inline void* RealSel() { return s_realSel; }

static void BuildShadow(void* real, const selmap::Map& m) {
    memset(s_shadow, 0, sizeof(s_shadow));
    const char* r = (const char*)real;
    *(int32_t*)(s_shadow + sh::STATE)     = *(const int32_t*)(r + m.state);
    /* No busy byte on PC: while the list animates (scroll/sort) the
     * selected row's panel still shows the previous pv, so "busy" =
     * displayed pv hasn't caught up with pv_id yet. Verified: it lags
     * for the whole ~100 ms transition on scrolls and re-sorts and
     * always converges at rest. A mismatch that persists (a row whose
     * panel never shows pv_id, e.g. random/empty) stops counting as busy
     * after 1 s so the follower lock and the launch autopilot can't hang. */
    static uint64_t s_busySince = 0;
    const bool lag = m.sel_disp_pv &&
        *(const int32_t*)(r + m.sel_disp_pv) != *(const int32_t*)(r + m.pv_id);
    const uint64_t nowMs = GetTickCount64();
    if (!lag) s_busySince = 0;
    else if (!s_busySince) s_busySince = nowMs;
    *(uint8_t*)(s_shadow + sh::BUSY)      =
        (uint8_t)(lag && nowMs - s_busySince < 1000);
    *(int32_t*)(s_shadow + sh::INDEX)     =
        m.index ? *(const int32_t*)(r + m.index) : 0;
    *(int32_t*)(s_shadow + sh::PV_ID)     = *(const int32_t*)(r + m.pv_id);
    *(int32_t*)(s_shadow + sh::SORT_MODE) = *(const int32_t*)(r + m.sort_mode);
    *(int32_t*)(s_shadow + sh::DIFF)      = *(const int32_t*)(r + m.difficulty);
    *(int32_t*)(s_shadow + sh::EDITION)   = *(const int32_t*)(r + m.edition);
    /* Copy the 32 vector headers so the lobby's sel_lists / iteration sees
     * live begin_/end_ pointers into the real elements. On FT the element
     * stride differs (0x50 vs 0x48) so leave them empty (n = -1). */
    if (!m.is_ft) {
        for (int i = 0; i < 32; i++) {
            auto* dst = (libcxx_vector_base<pvsel::PvData>*)(s_shadow + sh::LISTS + (size_t)i * 0x18);
            const auto* src = (const prj::vector<pvsel::PvData>*)(r + m.sorted_pv_lists + (size_t)i * 0x18);
            dst->begin_ = (pvsel::PvData*)src->begin_;
            dst->end_   = (pvsel::PvData*)src->end_;
            dst->cap_   = (pvsel::PvData*)src->cap_;
        }
    }
}

/* ------------------------------------------------------------------ */
/* mp entry points (MpNet.hpp)                                         */
/* ------------------------------------------------------------------ */
namespace mp {
void OnPvGameFrame();
void OnPvSelInit();
void OnPvSelDest();
void OnPvSelFrame(const void* sel);
void OnStageResult(bool init);
} // namespace mp

/* ------------------------------------------------------------------ */
/* PV game task hooks                                                  */
/* ------------------------------------------------------------------ */
HOOK(bool, __fastcall, TaskPvGameInit, addr::TaskPvGameInit, uint64_t a1) {
    return originalTaskPvGameInit(a1);
}
HOOK(bool, __fastcall, TaskPvGameCtrl, addr::TaskPvGameCtrl, uint64_t a1) {
    bool r = originalTaskPvGameCtrl(a1);
    AutoPlay::OnGameTaskFrame();
    mp::OnPvGameFrame();
    mp::input::SprTestDraw();
    return r;
}
HOOK(bool, __fastcall, TaskPvGameDest, addr::TaskPvGameDest, uint64_t a1) {
    g_current_game = nullptr;
    AutoPlay::OnGameEnd();
    return originalTaskPvGameDest(a1);
}

/* ------------------------------------------------------------------ */
/* Selector hooks (both UIs -> same mp callbacks via the shadow)        */
/* ------------------------------------------------------------------ */
static void SelInitCommon() { mp::OnPvSelInit(); }
static void SelDestCommon() { s_realSel = nullptr; s_map = nullptr; mp::OnPvSelDest(); }
static void SelCtrlCommon(void* sel, const selmap::Map& m) {
    s_realSel = sel;
    s_map = &m;
#ifdef MP_DEV_TOOLS
    static void* s_logSel = nullptr;
    if (sel != s_logSel) {
        s_logSel = sel;
        WriteLog("[MP] sel %p map=%s\n", sel, m.is_ft ? "FT" : "MM");
    }
#endif
    BuildShadow(sel, m);
    mp::OnPvSelFrame(s_shadow);
}

HOOK(bool, __fastcall, PvSelSwitchInit, addr::PvSelSwitchInit, uint64_t a1) {
    bool r = originalPvSelSwitchInit(a1);
    SelInitCommon();
    return r;
}
HOOK(bool, __fastcall, PvSelSwitchCtrl, addr::PvSelSwitchCtrl, void* a1) {
    bool r = originalPvSelSwitchCtrl(a1);
    SelCtrlCommon(a1, selmap::MM);
    return r;
}
HOOK(bool, __fastcall, PvSelSwitchDest, addr::PvSelSwitchDest, uint64_t a1) {
    bool r = originalPvSelSwitchDest(a1);
    SelDestCommon();
    return r;
}

HOOK(bool, __fastcall, PvSelPS4Init, addr::PvSelPS4Init, void* a1) {
    bool r = originalPvSelPS4Init(a1);
    SelInitCommon();
    return r;
}
HOOK(bool, __fastcall, PvSelPS4Ctrl, addr::PvSelPS4Ctrl, void* a1) {
    bool r = originalPvSelPS4Ctrl(a1);
    SelCtrlCommon(a1, selmap::FT);
    return r;
}
HOOK(bool, __fastcall, PvSelPS4Dest, addr::PvSelPS4Dest, uint64_t a1) {
    bool r = originalPvSelPS4Dest(a1);
    SelDestCommon();
    return r;
}

/* ------------------------------------------------------------------ */
/* Results hooks                                                       */
/* ------------------------------------------------------------------ */
HOOK(bool, __fastcall, StageResultSwitchInit, addr::StageResultSwitchInit, void* a1) {
    bool r = originalStageResultSwitchInit(a1);
    mp::OnStageResult(true);
    return r;
}
HOOK(bool, __fastcall, StageResultSwitchDest, addr::StageResultSwitchDest, void* a1) {
    bool r = originalStageResultSwitchDest(a1);
    mp::OnStageResult(false);
    return r;
}
HOOK(bool, __fastcall, StageResultPS4Init, addr::StageResultPS4Init, void* a1) {
    bool r = originalStageResultPS4Init(a1);
    mp::OnStageResult(true);
    return r;
}
HOOK(bool, __fastcall, StageResultPS4Dest, addr::StageResultPS4Dest, uint64_t a1) {
    bool r = originalStageResultPS4Dest(a1);
    mp::OnStageResult(false);
    return r;
}

/* Mirror result_switch.cpp: "win_vp_in" is the only menu-progress signal the
 * game gives for results page 2 — feed the blind menu model. */
HOOK(void, __fastcall, CAetInLoopOutSetLayer, addr::CAetControllerInLoopOutSetLayer,
     void* a1, const prj::string* in, const prj::string* loop,
     const prj::string* out, int32_t prio) {
    if (in && in->rfind("win_vp_in", 0) == 0) {
        uint64_t ms = (uint64_t)nn::os::GetSystemTick().GetInt64Value()
            * 1000 / nn::os::GetSystemTickFrequency();
        mp::menu::ResultsVpShown(ms);
    }
    originalCAetInLoopOutSetLayer(a1, in, loop, out, prio);
}

/* ------------------------------------------------------------------ */
/* Customize select (module picker) presence                           */
/* ------------------------------------------------------------------ */
static int s_customizeCount = 0;
HOOK(bool, __fastcall, CustomizeSelTaskInit, addr::CustomizeSelTaskInit, uint64_t a1) {
    bool r = originalCustomizeSelTaskInit(a1);
    s_customizeCount++;
    WriteLog("[MP] customize init (n=%d)\n", s_customizeCount);
    return r;
}
HOOK(bool, __fastcall, CustomizeSelTaskDest, addr::CustomizeSelTaskDest, uint64_t a1) {
    bool r = originalCustomizeSelTaskDest(a1);
    if (s_customizeCount > 0) s_customizeCount--;
    WriteLog("[MP] customize dest (n=%d)\n", s_customizeCount);
    return r;
}
namespace CustomizeSelUi {
bool IsInCustomizeScene() { return s_customizeCount > 0; }
bool IsOpen()             { return s_customizeCount > 0; }
}

/* ------------------------------------------------------------------ */
/* Input poll hook (block/inject at the InputState bitset level)        */
/* ------------------------------------------------------------------ */
namespace mp { namespace input {
    void Apply(diva::InputState* st);
    void ApplyPost(diva::InputState* st, bool notDispatchable);
} }
namespace mp { namespace lobby { void PumpPanelSE(); } }
HOOK(void, __fastcall, PollInputRepeatAndDouble, addr::PollInputRepeatAndDouble,
     diva::InputState* st, float a2) {
    originalPollInputRepeatAndDouble(st, a2);
    static std::atomic<int> s_calls{0};
    int n = s_calls.fetch_add(1);
    if (n < 3 || n == 600) WriteLog("[MP] input poll #%d st=%p\n", n, (void*)st);
    mp::input::Apply(st);
    mp::lobby::PumpPanelSE();
}

/* Last per-frame writer per InputState: the game may clear the live
 * bitsets here (dispatch gate — e.g. unfocused or modal state). Re-apply
 * the injected/filtered state after the original so injected keys reach
 * scenes regardless of focus. */
HOOK(void, __fastcall, SetInputNotDispatchable, addr::SetInputNotDispatchable,
     diva::InputState* st, bool blocked) {
    originalSetInputNotDispatchable(st, blocked);
    mp::input::ApplyPost(st, blocked);
}

/* ------------------------------------------------------------------ */
/* SongQuickJump (PC impl over the real selector)                       */
/* ------------------------------------------------------------------ */
static FUNCTION_PTR(bool, __fastcall, CheckSongPertainsMM, addr::PvSelSwitchPertains,
    void* sel, const pvsel::PvData* pv, int32_t, int32_t);
static FUNCTION_PTR(bool, __fastcall, CheckSongPertainsFT, addr::PvSelPS4Pertains,
    void* sel, const pvsel::PvDataPS4* pv, int32_t, int32_t);
static FUNCTION_PTR(bool, __fastcall, CreateSortedPVListMM, addr::PvSelSwitchCreateList, void* sel);
static FUNCTION_PTR(void, __fastcall, CreateSortedPVListFT, addr::PvSelPS4CreateList, void* sel);
static FUNCTION_PTR(void, __fastcall, ChangeSortFilterMM, addr::PvSelSwitchChangeSort, void* sel, int32_t dir);
static FUNCTION_PTR(void, __fastcall, ChangeSortFilterFT, addr::PvSelPS4ChangeSort, void* sel, int32_t dir);

static int32_t AllFolderForSort(int32_t mode) {
    switch (mode) { case 0: return 10; case 1: return 19;
                    case 2: return 6;  case 3: return 5; }
    return 19;
}

static bool PertainsElem(void* sel, const selmap::Map& m, const void* elem,
                         int32_t folder, int32_t folderDup) {
    if (m.is_ft)
        return CheckSongPertainsFT(sel, (const pvsel::PvDataPS4*)elem, folder, folderDup);
    return CheckSongPertainsMM(sel, (const pvsel::PvData*)elem, folder, folderDup);
}

static int32_t ElemPvId(const void* elem, const selmap::Map& m) {
    /* data2->pv_db_entry is at +0 in both element layouts */
    const pvsel::PvData* pd = (const pvsel::PvData*)elem;
    if (!pd->data2 || !pd->data2->pv_db_entry) return -1;
    return *pd->data2->pv_db_entry;
}

namespace SongQuickJump {

void Draw() {}
void ParsePvDbBuffer(const char*, size_t) {}
void SetPvSelActive(bool active, void* sel) { if (active) s_realSel = sel; else if (sel == s_realSel) s_realSel = nullptr; }
bool IsVisible() { return false; }

static int32_t SelMode(const void* sel, const selmap::Map& m) {
    int32_t mode = *(const int32_t*)((const char*)sel + m.sort_mode);
    if (mode < 0 || mode > 3) mode = 1;
    return mode;
}

/* what the selector actually shows: entry in the sorted list AND passes the
 * All-folder pertains check (nc also ANDs an nc-style check — not used here) */
static bool EntryShown(void* sel, const selmap::Map& m, const void* elem,
                       int32_t mode, int32_t diff, int32_t ed) {
    int32_t pvId = ElemPvId(elem, m);
    if (pvId <= 0) return false;
    int32_t folder = AllFolderForSort(mode);
    (void)diff; (void)ed;
    return PertainsElem(sel, m, elem, folder, folder);
}

bool PvSelectableAt(const void* selPtr, int32_t pv, int32_t diff, int32_t ed) {
    void* sel = RealSel();
    const selmap::Map* mp = s_map;
    if (!sel || !mp || pv <= 0) return false;
    int32_t mode = SelMode(sel, *mp);
    if (mode < 0 || mode > 3 || diff < 0 || diff > 3 || ed < 0 || ed > 1)
        return false;
    const auto* vec = (const libcxx_vector_base<uint8_t>*)
        ((const char*)sel + mp->sorted_pv_lists + (size_t)(8 * mode + 2 * diff + ed) * 0x18);
    if (!vec->begin_ || vec->end_ < vec->begin_ ||
        (vec->end_ - vec->begin_) / (ptrdiff_t)mp->elem_size > 5000)
        return false;
    int n = (int)((vec->end_ - vec->begin_) / (ptrdiff_t)mp->elem_size);
    for (int i = 0; i < n; i++) {
        const uint8_t* elem = vec->begin_ + (size_t)i * mp->elem_size;
        if (ElemPvId(elem, *mp) == pv)
            return EntryShown(sel, *mp, elem, mode, diff, ed);
    }
    return false;
}

int32_t SelVisibleCount(const void* selPtr, int32_t diff, int32_t ed) {
    void* sel = RealSel();
    const selmap::Map* mp = s_map;
    if (!sel || !mp) return -1;
    int32_t mode = SelMode(sel, *mp);
    if (mode < 0 || mode > 3 || diff < 0 || diff > 3 || ed < 0 || ed > 1)
        return -1;
    const auto* vec = (const libcxx_vector_base<uint8_t>*)
        ((const char*)sel + mp->sorted_pv_lists + (size_t)(8 * mode + 2 * diff + ed) * 0x18);
    if (!vec->begin_ || vec->end_ < vec->begin_) return -1;
    int n = (int)((vec->end_ - vec->begin_) / (ptrdiff_t)mp->elem_size), cnt = 0;
    for (int i = 0; i < n; i++)
        if (EntryShown(sel, *mp, vec->begin_ + (size_t)i * mp->elem_size, mode, diff, ed))
            cnt++;
    return cnt;
}

bool ClosestSelectable(const void* selPtr, int32_t pv, int32_t curDiff,
                       int32_t curEd, int32_t* outDiff, int32_t* outEd) {
    int32_t cand[14][2];
    int nc = 0;
    cand[nc][0] = curDiff; cand[nc][1] = 1 - curEd; nc++;
    for (int k = 1; k <= 3; k++) {
        for (int dir = -1; dir <= 1; dir += 2) {
            int32_t d = curDiff + k * dir;
            if (d < 0 || d > 3) continue;
            for (int e = 0; e < 2; e++) {
                cand[nc][0] = d; cand[nc][1] = e; nc++;
            }
        }
    }
    for (int i = 0; i < nc; i++) {
        if (PvSelectableAt(selPtr, pv, cand[i][0], cand[i][1])) {
            *outDiff = cand[i][0]; *outEd = cand[i][1];
            return true;
        }
    }
    return false;
}

static bool JumpImpl(void* sel, const selmap::Map& m, int32_t pvId,
                     int32_t difficulty, int32_t edition,
                     bool writeDifficulty) {
    if (!sel || pvId <= 0) return false;
    char* s = (char*)sel;
    int32_t mode = *(int32_t*)(s + m.sort_mode);
    if (mode < 0 || mode > 3) mode = 1;
    int32_t all_folder = AllFolderForSort(mode);

    if (writeDifficulty) {
        *(int32_t*)(s + m.difficulty) = difficulty;
        *(int32_t*)(s + m.edition)    = edition;
    } else {
        difficulty = *(int32_t*)(s + m.difficulty);
        edition    = *(int32_t*)(s + m.edition);
    }

    int32_t list_idx = (8 * mode) + (2 * difficulty) + edition;
    *(uintptr_t*)(s + m.cur_pv_list) = (uintptr_t)(s + m.sorted_pv_lists + (size_t)list_idx * 0x18);

    if (m.is_ft) CreateSortedPVListFT(sel); else CreateSortedPVListMM(sel);

    *(int32_t*)(s + m.pv_id) = pvId;
    int32_t* folder_ptr = nullptr;
    if (m.sort_indices) {
        *(int32_t*)(s + m.sort_indices + mode * 4) = all_folder;
        folder_ptr = (int32_t*)(s + m.sort_indices + mode * 4);
        *(int32_t**)(s + m.cur_sort_index) = folder_ptr;
    } else {
        folder_ptr = *(int32_t**)(s + m.cur_sort_index);
        if (folder_ptr) *folder_ptr = all_folder;
    }
    if (m.sort_change_dir)
        *(int32_t*)(s + m.sort_change_dir) = 1;

    diva::GlobalPVInfo* g = diva::game::GetGlobalPVInfo();
    if (g) {
        g->pv_id = pvId;
        if (writeDifficulty) g->difficulty = difficulty;
    }

    /* SelPvList internal "list changed" flags (Switch +0x68/+0x69) */
    *(uint8_t*)(s + m.sel_pv_list + 0x68) = 0;
    *(uint8_t*)(s + m.sel_pv_list + 0x69) = 1;

    if (m.is_ft) ChangeSortFilterFT(sel, 0); else ChangeSortFilterMM(sel, 0);
    return true;
}

bool JumpSelectorToPv(void* selPtr, int32_t pvId, bool keepDifficulty) {
    void* sel = RealSel();
    if (!sel || !s_map) return false;
    return JumpImpl(sel, *s_map, pvId, 3, 0, !keepDifficulty);
}

bool JumpSelectorToPvAt(void* selPtr, int32_t pvId, int32_t diff, int32_t ed) {
    void* sel = RealSel();
    if (!sel || !s_map) return false;
    return JumpImpl(sel, *s_map, pvId, diff, ed, true);
}

/* --- pv_db-backed song list (list_pvs / FindPvByName / PvName) --- */
struct SongEntry { int32_t id; std::string name_en; std::string name_jp; };
static std::vector<SongEntry> s_songDb;
static bool s_dbLoaded = false;

static void LoadSongDb() {
    if (s_dbLoaded) return;
    s_dbLoaded = true;
    for (int32_t id = 1; id < 1000; id++) {
        diva::pv_db::PvDBEntry* e = diva::pv_db::FindPVEntry(id);
        if (!e) continue;
        SongEntry se{};
        se.id = id;
        se.name_en = e->song_name;
        se.name_jp = e->song_name_reading;
        s_songDb.push_back(std::move(se));
    }
    WriteLog("[MP] song db: %d entries\n", (int)s_songDb.size());
}

bool GetSongDbEntry(int i, int32_t* id, char* name, size_t nameCap) {
    LoadSongDb();
    if (i < 0 || (size_t)i >= s_songDb.size()) return false;
    *id = s_songDb[i].id;
    const std::string& n = s_songDb[i].name_en.empty()
        ? s_songDb[i].name_jp : s_songDb[i].name_en;
    size_t ln = n.size();
    if (ln > 24) ln = 24;
    if (ln + 1 > nameCap) ln = nameCap - 1;
    memcpy(name, n.data(), ln);
    name[ln] = 0;
    return true;
}

static std::string ToLowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}

int32_t FindPvByName(const char* name) {
    if (!name || !*name) return -1;
    LoadSongDb();
    std::string q = name;
    std::string lq = ToLowerAscii(q);
    for (const auto& e : s_songDb) {
        if (!e.name_en.empty() && ToLowerAscii(e.name_en) == lq) return e.id;
        if (e.name_jp == q) return e.id;
    }
    for (const auto& e : s_songDb) {
        if (!e.name_en.empty() &&
            ToLowerAscii(e.name_en).find(lq) != std::string::npos) return e.id;
        if (!e.name_jp.empty() &&
            e.name_jp.find(q) != std::string::npos) return e.id;
    }
    return -1;
}

bool PvInDb(int32_t pv) {
    if (pv <= 0) return false;
    return diva::pv_db::FindPVEntry(pv) != nullptr;
}

bool PvName(int32_t pv, char* name, size_t nameCap) {
    LoadSongDb();
    for (const auto& e : s_songDb) {
        if (e.id == pv) {
            const std::string& n = e.name_en.empty() ? e.name_jp : e.name_en;
            size_t ln = n.size();
            if (ln + 1 > nameCap) ln = nameCap - 1;
            memcpy(name, n.data(), ln);
            name[ln] = 0;
            return true;
        }
    }
    return false;
}

} // namespace SongQuickJump

/* ------------------------------------------------------------------ */
/* Install                                                             */
/* ------------------------------------------------------------------ */
void InstallMpGameHooks() {
    INSTALL_HOOK(TaskPvGameInit);
    INSTALL_HOOK(TaskPvGameCtrl);
    INSTALL_HOOK(TaskPvGameDest);
    INSTALL_HOOK(PvSelSwitchInit);
    INSTALL_HOOK(PvSelSwitchCtrl);
    INSTALL_HOOK(PvSelSwitchDest);
    INSTALL_HOOK(PvSelPS4Init);
    INSTALL_HOOK(PvSelPS4Ctrl);
    INSTALL_HOOK(PvSelPS4Dest);
    INSTALL_HOOK(StageResultSwitchInit);
    INSTALL_HOOK(StageResultSwitchDest);
    INSTALL_HOOK(StageResultPS4Init);
    INSTALL_HOOK(StageResultPS4Dest);
    INSTALL_HOOK(CAetInLoopOutSetLayer);
    INSTALL_HOOK(CustomizeSelTaskInit);
    INSTALL_HOOK(CustomizeSelTaskDest);
    INSTALL_HOOK(PollInputRepeatAndDouble);
    INSTALL_HOOK(SetInputNotDispatchable);
    AutoPlay::init();
    WriteLog("[MP] game hooks installed\n");
}
