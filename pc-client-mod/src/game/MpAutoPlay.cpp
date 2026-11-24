/* PC AutoPlay: vanilla-MM+ reimplementation of the Switch AutoPlay
 * (switch-client-mod/source/program/AutoPlay.cpp). Same modes, same
 * deterministic HitPlan, same indicator — but driven by the game's own
 * PvGameTarget linked list and GetPvKeyState{Down,Tapped} hooks instead
 * of New Classics' target_references/macro_state (not present on PC).
 *
 * Input model (PC, verified on MM+ 1.03): GetPvKeyStateDown(0x140274930)
 * /Tapped(0x140274960) return int64 key masks from handler+0x68/+0xA8
 * when *(u8*)(PVGameData+0x2D379)==0 && key<8. Handler key indexes:
 * Circle 0, Cross 1, Triangle 2, Square 3, L 4, R 7; bits Triangle 1,
 * Square 4, Cross 16, Circle 64, L1 256 (Q), R1 4096 (E). We OR our
 * per-frame masks in — planned each frame by Update(), called from the
 * arcade-ctrl entry mid-hook before that frame's key reads. */
#include "MpAutoPlay.hpp"
#include "diva.h"
#include "addresses.h"
#include "../platform/NxShim.hpp"
#include "../platform/MpLog.hpp"
#include <imgui.h>
#include <atomic>
#include <cfloat>
#include <cstring>
#include <cstdio>

namespace AutoPlay {

bool g_enabled = false;
int32_t g_hitPct = 100;
int32_t g_mode = Mode_Custom;

/* Judgment windows (vanilla, same values as nc hit_state.cpp NormalWindow):
 * Cool +-0.03, Fine +-0.07, Safe +-0.10, Sad +-0.13 s. A mode "degrades" a
 * note by injecting the press late; the press lands ~8-17 ms after the
 * inject threshold (1 frame), inside the intended band. */
static constexpr float kLateFine = 0.055f;   /* inject at <= -0.047 -> ~Fine */
static constexpr float kLateSafe = 0.090f;   /* inject at <= -0.082 -> ~Safe */
static constexpr float kLateSad  = 0.112f;   /* inject at <= -0.104 -> ~Sad  */
static constexpr float kGiveUp   = -0.125f;  /* past this, don't bother      */
static constexpr float kRelBefore = 0.040f;  /* release a hold ~2 frames early
                                              * when the same button is needed
                                              * for another note             */

static const char* const kTags[Mode_Count] = {
    "", "AUTO", "AUTO PRO", "AUTO 97",
    "AUTO STREAK", "AUTO HUMAN", "AUTO CLUTCH", "AUTO TIE"
};

static const char* const kNames[Mode_Count] = {
    "custom", "auto", "pro", "97",
    "streak", "human", "clutch", "tie"
};

const char* ModeTag() {
    static char buf[24];
    if (g_mode == Mode_Custom) {
        if (g_hitPct >= 100) snprintf(buf, sizeof(buf), "AUTO");
        else snprintf(buf, sizeof(buf), "AUTO %d%%", (int)g_hitPct);
        return buf;
    }
    int32_t m = (g_mode >= 0 && g_mode < Mode_Count) ? g_mode : Mode_Auto;
    return kTags[m];
}

const char* ModeName() {
    int32_t m = (g_mode >= 0 && g_mode < Mode_Count) ? g_mode : Mode_Custom;
    return kNames[m];
}

int32_t EffectivePct() {
    switch (g_mode) {
        case Mode_Pro:    return 97;
        case Mode_97:     return 97;
        case Mode_Streak: return 94;
        case Mode_Human:  return 95;
        case Mode_Clutch: return 95;
        case Mode_Tie:    return 95;
        case Mode_Custom: return g_hitPct;
        default:          return 100;
    }
}

int32_t ModeFromName(const char* s) {
    if (!s) return -1;
    if (!strcmp(s, "auto"))   return Mode_Auto;
    if (!strcmp(s, "pro"))    return Mode_Pro;
    if (!strcmp(s, "97") || !strcmp(s, "90")) return Mode_97;
    if (!strcmp(s, "streak")) return Mode_Streak;
    if (!strcmp(s, "human"))  return Mode_Human;
    if (!strcmp(s, "clutch")) return Mode_Clutch;
    if (!strcmp(s, "tie"))    return Mode_Tie;
    if (!strcmp(s, "custom") || !strcmp(s, "pct")) return Mode_Custom;
    return -1;
}

static uint64_t NowMs() {
    return nn::os::GetSystemTick().GetInt64Value() * 1000 /
           nn::os::GetSystemTickFrequency();
}

static std::atomic<int64_t> s_gameFrameMs{0};

void OnGameTaskFrame() {
    s_gameFrameMs.store((int64_t)NowMs(), std::memory_order_relaxed);
}

bool IndicatorVisible() {
    if (!g_enabled || diva::game::IsInSongResults()) return false;
    return (int64_t)NowMs() - s_gameFrameMs.load(std::memory_order_relaxed)
           < 300;
}

void DrawIndicator() {
    if (!IndicatorVisible()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;
    const ImGuiIO& io = ImGui::GetIO();
    float k = io.DisplaySize.y / 720.0f;
    const char* txt = ModeTag();
    float fs = ImGui::GetFont()->FontSize * 1.15f;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, txt);
    float padX = 8 * k, padY = 3 * k;
    ImVec2 p0(14 * k, 62 * k);
    ImVec2 p1(p0.x + ts.x + padX * 2, p0.y + ts.y + padY * 2);
    dl->AddRectFilled(p0, p1, IM_COL32(255, 201, 51, 235), 6 * k);
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 120), 6 * k, 0, 1.5f * k);
    dl->AddText(ImGui::GetFont(), fs,
                ImVec2(p0.x + padX, p0.y + padY),
                IM_COL32(25, 25, 25, 255), txt);
}

/* ------------------------------------------------------------------ */
/* Planning                                                            */
/* ------------------------------------------------------------------ */

/* vanilla target_type -> face button: { handler key index, key bit }.
 * Base type = target_type % 4 for 0..7 and target_type - 18 for 18..21:
 *   0 Triangle -> key 2 bit 1, 1 Circle -> key 0 bit 64,
 *   2 Cross -> key 1 bit 16, 3 Square -> key 3 bit 4.                 */
struct BtnMap { int32_t index; int64_t mask; };
static const BtnMap kBaseBtn[4] = { {2, 1}, {0, 64}, {1, 16}, {3, 4} };
/* slides: L1 = key 4 bit 256, R1 = key 7 bit 4096 */
static const BtnMap kSlide[2]   = { {4, 256}, {7, 4096} };   /* 0=L, 1=R */

static int64_t s_auto_down[8]   = {};
static int64_t s_auto_tapped[8] = {};
static int64_t s_held_bits[8]   = {};   /* crosses frames: holds/chains */

static diva::PvGameTarget* s_holdTgt[4] = {};   /* face holds, by base   */
static bool    s_chainHold[2] = {};             /* chain slides L/R      */
static diva::PVGameArcade* s_lastGame = nullptr;

#ifdef MP_DEV_TOOLS
static uint64_t s_seenTypes = 0;
#endif

static void TapButton(int32_t key_index, int64_t key_mask) {
    s_auto_tapped[key_index] |= key_mask;
    s_auto_down[key_index]   |= key_mask;
}

static void HoldButton(int32_t key_index, int64_t key_mask) {
    s_held_bits[key_index] |= key_mask;
    s_auto_down[key_index] |= key_mask;
}

static void ReleaseButton(int32_t key_index, int64_t key_mask) {
    s_held_bits[key_index] &= ~key_mask;
    s_auto_down[key_index] &= ~key_mask;
}

static void ResetState() {
    memset(s_auto_down, 0, sizeof(s_auto_down));
    memset(s_auto_tapped, 0, sizeof(s_auto_tapped));
    memset(s_held_bits, 0, sizeof(s_held_bits));
    memset(s_holdTgt, 0, sizeof(s_holdTgt));
    s_chainHold[0] = s_chainHold[1] = false;
}

void OnGameEnd() {
    s_lastGame = nullptr;
    ResetState();
}

/* targets live in game->target_buffer[64]; reject anything else so a
 * stale/wild next pointer can never send the walks off into space */
static bool ValidTgt(diva::PVGameArcade* game, const diva::PvGameTarget* t) {
    uintptr_t a = reinterpret_cast<uintptr_t>(game->target_buffer);
    uintptr_t p = reinterpret_cast<uintptr_t>(t);
    return p >= a && p < a + sizeof(game->target_buffer);
}

static bool InList(diva::PVGameArcade* game, const diva::PvGameTarget* t) {
    int steps = 0;
    for (const diva::PvGameTarget* p = game->target;
         p && ValidTgt(game, p) && steps < 80; p = p->next, steps++)
        if (p == t) return true;
    return false;
}

static int32_t TotalTargetIndex(diva::PVGameArcade* game) {
    int32_t mx = 0, steps = 0;
    for (diva::PvGameTarget* t = game->target;
         t && ValidTgt(game, t) && steps < 80; t = t->next, steps++)
        if (t->target_index > mx) mx = t->target_index;
    return mx + 1;
}

/* deterministic per target index: >= 0 = hit, injected that many seconds
 * late (degraded judgment); < 0 = skip (real miss). Same chart + same
 * mode on two consoles -> identical judgments -> tie. */
static float HitPlan(int32_t idx, int32_t total) {
    if (idx < 0) return 0.0f;
    switch (g_mode) {
    case Mode_Pro:
        /* ~1/32 notes get a degraded judgment, biased away from outright
         * misses: ~50 % Fine, ~31 % Safe, ~13 % Sad, ~6 % miss */
        if ((idx % 32) == 20) {
            uint32_t h = ((uint32_t)idx * 7919u) >> 28;   /* 0..15 */
            if (h < 8)  return kLateFine;
            if (h < 13) return kLateSafe;
            if (h < 15) return kLateSad;
            return -1.0f;
        }
        return 0.0f;
    case Mode_97:
        return (idx % 32) == 20 ? -1.0f : 0.0f;
    case Mode_Streak:
        return (idx % 32) >= 30 ? -1.0f : 0.0f;
    case Mode_Tie:
        if (idx % 9 == 0) return kLateFine;
        if (idx % 9 == 4) return kLateSafe;
        return 0.0f;
    case Mode_Human: {
        uint32_t h = ((uint32_t)idx * 2654435761u) >> 24;   /* 0..255 */
        if (h < 204) return 0.0f;        /* ~80 % Cool */
        if (h < 240) return kLateFine;   /* ~14 % Fine */
        if (h < 250) return kLateSafe;   /* ~4 % Safe  */
        if (h < 253) return kLateSad;    /* ~1 % Sad   */
        return -1.0f;                    /* ~1 % miss  */
    }
    case Mode_Clutch: {
        float f = total > 1 ? (float)idx / (float)total : 1.0f;
        if (f > 1.0f) f = 1.0f;
        uint32_t h = ((uint32_t)idx * 2654435761u) >> 24;
        int32_t cut = 165 + (int32_t)(90.0f * f);   /* ~65 % Cool early -> 100 % */
        if ((int32_t)h < cut)      return 0.0f;
        if ((int32_t)h < cut + 20) return kLateFine;
        if ((int32_t)h < 248)      return kLateSafe;
        return -1.0f;
    }
    case Mode_Custom:
        if (g_hitPct >= 100) return 0.0f;
        if (g_hitPct <= 0)   return -1.0f;
        return ((int64_t)idx * g_hitPct) % 100 < g_hitPct ? 0.0f : -1.0f;
    case Mode_Auto:
    default:
        return 0.0f;
    }
}

/* nearest pending (hit_state == None) target of face base button `b`,
 * excluding `excl`; nullptr if none */
static diva::PvGameTarget* NearestPendingBase(diva::PVGameArcade* game,
                                              int32_t b,
                                              const diva::PvGameTarget* excl) {
    diva::PvGameTarget* best = nullptr;
    int steps = 0;
    for (diva::PvGameTarget* t = game->target;
         t && ValidTgt(game, t) && steps < 80; t = t->next, steps++) {
        if (t == excl || t->hit_state != HitState_None) continue;
        int32_t ty = t->target_type;
        int32_t base = (ty >= 18 && ty <= 21) ? ty - 18
                     : (ty >= 0 && ty <= 7)   ? ty % 4 : -1;
        if (base != b) continue;
        if (!best || t->flying_time_remaining < best->flying_time_remaining)
            best = t;
    }
    return best;
}

/* nearest pending slide of direction d (0=L, 1=R); chained pieces may be
 * excluded via `chainsOnly`/!`chainsOnly` selection */
static diva::PvGameTarget* NearestPendingSlide(diva::PVGameArcade* game,
                                               int32_t d, bool chainsOnly) {
    diva::PvGameTarget* best = nullptr;
    int steps = 0;
    for (diva::PvGameTarget* t = game->target;
         t && ValidTgt(game, t) && steps < 80; t = t->next, steps++) {
        if (t->hit_state != HitState_None) continue;
        int32_t ty = t->target_type;
        bool slide = t->slide_left || t->slide_right || t->slide_chain ||
                     (ty >= 12 && ty <= 16) || (ty == 23 || ty == 24);
        if (!slide || t->slide_chain != chainsOnly) continue;
        bool dir = (d == 0) ? (t->slide_left  || ty == 12 || ty == 15 || ty == 23)
                            : (t->slide_right || ty == 13 || ty == 16 || ty == 24);
        if (!dir) continue;
        if (!best || t->flying_time_remaining < best->flying_time_remaining)
            best = t;
    }
    return best;
}

void Update(diva::PVGameArcade* game) {
    if (game != s_lastGame) {
        s_lastGame = game;
        ResetState();
#ifdef MP_DEV_TOOLS
        s_seenTypes = 0;
#endif
    }
    memcpy(s_auto_down, s_held_bits, sizeof(s_auto_down));
    memset(s_auto_tapped, 0, sizeof(s_auto_tapped));
    if (!g_enabled || !game || !game->target || !game->play)
        return;

#ifdef MP_DEV_TOOLS
    {
        static uint64_t s_nextDump = 0;
        uint64_t now = NowMs();
        if (now >= s_nextDump) {
            s_nextDump = now + 400;
            int n = 0; int hsSeen = -1;
            for (diva::PvGameTarget* t = game->target;
                 t && ValidTgt(game, t) && n < 80; t = t->next, n++) {
                if (n < 6)
                    WriteLog("[AP] l%d ty=%d ftr=%.3f hs=%d mc=%d idx=%d "
                             "act=%d s=(%d%d%d%d%d)\n", n, t->target_type,
                             t->flying_time_remaining, t->hit_state,
                             t->multi_count, t->target_index,
                             t->note_active ? 1 : 0,
                             t->slide_chain_start ? 1 : 0,
                             t->slide_chain_end ? 1 : 0,
                             t->slide_left ? 1 : 0,
                             t->slide_right ? 1 : 0,
                             t->slide_chain ? 1 : 0);
                if (t->hit_state == HitState_None) hsSeen = n;
            }
            WriteLog("[AP] list n=%d noneAt=%d\n", n, hsSeen);
        }
    }
#endif

    const int32_t total = TotalTargetIndex(game);

    /* 1. sustain face-button holds; drop when the held note resolves, and
     * release ~2 frames before the same button is needed again */
    for (int b = 0; b < 4; b++) {
        diva::PvGameTarget* h = s_holdTgt[b];
        if (!h) continue;
        const BtnMap& bb = kBaseBtn[b];
        bool keep = InList(game, h) && h->note_active;
        if (keep) {
            diva::PvGameTarget* nxt = NearestPendingBase(game, b, h);
            if (nxt && nxt->flying_time_remaining < kRelBefore &&
                HitPlan(nxt->target_index, total) >= 0.0f)
                keep = false;
        }
        if (!keep) { s_holdTgt[b] = nullptr; ReleaseButton(bb.index, bb.mask); }
    }

    /* 2. sustain chain slides: held from the first piece while pieces of
     * that direction are still pending; release for a stray same-side
     * slide (never for the chain's own remaining pieces) */
    for (int d = 0; d < 2; d++) {
        if (!s_chainHold[d]) continue;
        const BtnMap& sb = kSlide[d];
        bool keep = NearestPendingSlide(game, d, true) != nullptr;
        if (keep) {
            diva::PvGameTarget* nxt = NearestPendingSlide(game, d, false);
            if (nxt && nxt->flying_time_remaining < kRelBefore &&
                HitPlan(nxt->target_index, total) >= 0.0f)
                keep = false;
        }
        if (!keep) { s_chainHold[d] = false; ReleaseButton(sb.index, sb.mask); }
    }

    /* 3. closest upcoming target — vanilla types incl. slides (unlike the
     * Switch version, which pushed slides through nc's own judge path) */
    diva::PvGameTarget* first = nullptr;
    int steps = 0;
    for (diva::PvGameTarget* t = game->target;
         t && ValidTgt(game, t) && steps < 80; t = t->next, steps++) {
        if (t->hit_state != HitState_None) continue;
        if (!first || t->target_index < first->target_index) first = t;
    }
    if (!first) return;

    float plan = HitPlan(first->target_index, total);
    float want = plan > 0.0f ? plan : 0.0f;
    float time_rem = first->flying_time_remaining;
    if (time_rem > 0.008f - want || time_rem < kGiveUp) return;
    const bool hit = plan >= 0.0f;
    if (!hit) return;   /* a skipped chord counts as a miss */
#ifdef MP_DEV_TOOLS
    static int s_tapLog = 0;
    if (s_tapLog < 60) {
        s_tapLog++;
        WriteLog("[AP] tap idx=%d ty=%d ftr=%.3f hs=%d mc=%d\n",
                 first->target_index, first->target_type, time_rem,
                 first->hit_state, first->multi_count);
    }
#endif

    /* 4. tap the target's chord (multi_count < 0 = single note; members
     * share the value, so all of them are tapped on this same frame) */
    steps = 0;
    for (diva::PvGameTarget* t = game->target;
         t && ValidTgt(game, t) && steps < 80; t = t->next, steps++) {
        if (t->hit_state != HitState_None) continue;
        if (t != first &&
            !(first->multi_count >= 0 && t->multi_count == first->multi_count))
            continue;

        int32_t ty = t->target_type;
#ifdef MP_DEV_TOOLS
        if (ty >= 0 && ty < 64 && !(s_seenTypes & (1ull << ty))) {
            s_seenTypes |= 1ull << ty;
            WriteLog("[AP] tgt type %d idx %d chain=%d l=%d r=%d multi=%d\n",
                     ty, t->target_index, t->slide_chain ? 1 : 0,
                     t->slide_left ? 1 : 0, t->slide_right ? 1 : 0,
                     t->multi_count);
        }
#endif
        if (ty >= 0 && ty <= 3) {                      /* tap notes */
            const BtnMap& bb = kBaseBtn[ty % 4];
            TapButton(bb.index, bb.mask);
        } else if (ty >= 4 && ty <= 7) {               /* hold notes */
            int32_t b = ty % 4;
            const BtnMap& bb = kBaseBtn[b];
            if (s_holdTgt[b] && s_holdTgt[b] != t)
                ReleaseButton(bb.index, bb.mask);      /* edge: end old */
            s_holdTgt[b] = t;
            HoldButton(bb.index, bb.mask);
            TapButton(bb.index, bb.mask);
        } else if (ty >= 18 && ty <= 21) {             /* chance notes */
            const BtnMap& bb = kBaseBtn[ty - 18];
            TapButton(bb.index, bb.mask);
        } else {
            bool left  = t->slide_left  || ty == 12 || ty == 15 || ty == 23;
            bool right = t->slide_right || ty == 13 || ty == 16 || ty == 24;
            bool slide = left || right;
            if (slide) {
                if (left) {
                    TapButton(kSlide[0].index, kSlide[0].mask);
                    if (t->slide_chain) {
                        s_chainHold[0] = true;
                        HoldButton(kSlide[0].index, kSlide[0].mask);
                    }
                }
                if (right) {
                    TapButton(kSlide[1].index, kSlide[1].mask);
                    if (t->slide_chain) {
                        s_chainHold[1] = true;
                        HoldButton(kSlide[1].index, kSlide[1].mask);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

/* The ctrl keeps rcx (handler), r8, r9, r11 live across GetPvKeyState*
 * calls and the originals happen to preserve them; a C++ detour cannot.
 * The MASM shims (MpAutoPlayAsm.asm) save all volatile regs + xmm0-5
 * around these impls so the hooked functions behave identically. */
extern "C" int64_t ApKeyDownDetour(void* handler, int32_t key);
extern "C" int64_t ApKeyTappedDetour(void* handler, int32_t key);

static SafetyHookInline s_kDownHook;
static SafetyHookInline s_kTapHook;

static bool InjectAllowed() {
    if (!g_enabled || !g_current_game) return false;
    diva::PVGameData* pv = diva::game::GetPVGameData();
    /* the original only reads the handler arrays when this byte is 0 */
    return pv && *reinterpret_cast<const uint8_t*>(
                     reinterpret_cast<const char*>(pv) + 0x2D379) == 0;
}

extern "C" int64_t ApKeyDownImpl(void* handler, int32_t key) {
    int64_t orig = s_kDownHook.call<int64_t>(handler, key);
    if (key >= 0 && key < 8 && InjectAllowed())
        orig |= s_auto_down[key];
    return orig;
}

extern "C" int64_t ApKeyTappedImpl(void* handler, int32_t key) {
    int64_t orig = s_kTapHook.call<int64_t>(handler, key);
    if (key >= 0 && key < 8 && InjectAllowed()) {
        orig |= s_auto_tapped[key];
#ifdef MP_DEV_TOOLS
        if (s_auto_tapped[key]) {
            static int s_injLog = 0;
            if (s_injLog < 60) {
                s_injLog++;
                WriteLog("[AP] inj key=%d mask=0x%llx orig=0x%llx\n",
                         key, (unsigned long long)s_auto_tapped[key],
                         (unsigned long long)(orig & ~s_auto_tapped[key]));
            }
        }
#endif
    }
    return orig;
}

/* arcade controller entry: rcx = PVGameArcade*, xmm1 = dt. Mid-hooked so
 * g_current_game is live for the whole frame and Update() runs before
 * every key read of that frame (KeyDown/KeyTap loops, then GetHitState,
 * then UpdateTargets). */
MIDASM_HOOK(PvArcadeCtrlEntry, addr::PvArcadeCtrl) {
    g_current_game = reinterpret_cast<diva::PVGameArcade*>(ctx.rcx);
#ifdef MP_DEV_TOOLS
    static int s_diag = 0;
    if (s_diag < 3 && g_current_game) {
        diva::PvGameTarget* t = g_current_game->target;
        WriteLog("[AP] ctrl#%d game=%p play=%d target=%p inbuf=%d\n", s_diag,
                 (void*)g_current_game, g_current_game->play ? 1 : 0,
                 (void*)t, t ? (ValidTgt(g_current_game, t) ? 1 : 0) : -1);
        s_diag++;
    }
#endif
    Update(g_current_game);
}

void init() {
    s_kDownHook = safetyhook::create_inline(
        reinterpret_cast<void*>(addr::GetPvKeyStateDown),
        reinterpret_cast<void*>(&ApKeyDownDetour));
    s_kTapHook = safetyhook::create_inline(
        reinterpret_cast<void*>(addr::GetPvKeyStateTapped),
        reinterpret_cast<void*>(&ApKeyTappedDetour));
    INSTALL_MIDASM_HOOK(PvArcadeCtrlEntry);
}

} // namespace AutoPlay
