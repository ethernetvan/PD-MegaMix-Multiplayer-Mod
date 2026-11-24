#include "MpInput.hpp"
#include "MpMenu.hpp"
#include <nn.hpp>
#include <mutex>
#include <cstring>

namespace mp::input {

struct TapEntry {
    uint64_t buttons;
    uint32_t hold_ms;
    uint32_t gap_ms;
    bool     force;
};

static TapEntry    s_taps[32];
static uint32_t    s_tapHead = 0, s_tapCount = 0;
static uint64_t    s_tapStartTick = 0;
static std::mutex  s_mtx;
static uint64_t    s_blocked = 0;
static bool        s_zeroSticks = false;
static int         s_bypassDepth = 0;   /* set while our own UI polls pad */
static uint64_t    s_bypassTid = 0;
/* per-source real (pre-mask) buttons + last-seen ms: Apply runs once per
 * GetNpadState hook (Handheld/FullKey/JoyDual) and only some sources carry
 * a given button, so a single shared prev makes a held button look like a
 * fresh edge on every poll. OR the sources seen within 250 ms instead. */
/* per-(style, controller-slot) real-input state: 3 styles x 9 slots
 * (ids 0..7 plus one slot for the handheld id 0x20). Two controllers of
 * the same style must not share a slot - an idle one would overwrite the
 * pressed one's state and every re-appearance would look like a new edge. */
static uint64_t    s_srcReal[27] = {};
static uint64_t    s_srcRealMs[27] = {};
static uint64_t    s_prevRealComb = 0;
static uint64_t    s_realEdges = 0;     /* latched rising edges, real input */
static uint64_t    s_navHeld = 0;       /* nav: real|inject, pre-block */
static uint64_t    s_navPrev = 0;
static uint64_t    s_navEdges = 0;      /* latched nav rising edges */
static int32_t     s_navLx = 0, s_navLy = 0;

/* TPIDRRO_EL0 is unique per thread on Horizon. */
static inline uint64_t CurTid() {
    uint64_t v;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(v));
    return v;
}

static struct {
    bool     active = false;
    uint64_t buttons = 0;
    uint32_t hz = 0;
    uint64_t startTick = 0;
    uint64_t endTick = 0;
} s_spam;

static uint64_t NowTick() {
    return nn::os::GetSystemTick().GetInt64Value();
}
static uint64_t MsToTick(uint64_t ms) {
    return ms * nn::os::GetSystemTickFrequency() / 1000;
}

void Tap(uint64_t buttons, uint32_t hold_ms, uint32_t gap_ms, bool force) {
    if (buttons == 0) return;
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_tapCount == 32) {                  /* full: drop oldest */
        s_tapHead = (s_tapHead + 1) & 31;
        s_tapCount--;
    }
    uint32_t idx = (s_tapHead + s_tapCount) & 31;
    s_taps[idx] = {buttons, hold_ms, gap_ms, force};
    if (s_tapCount == 0) s_tapStartTick = NowTick();
    s_tapCount++;
}

void Spam(uint64_t buttons, uint32_t hz, uint32_t secs) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (hz == 0 || secs == 0 || buttons == 0) {
        s_spam.active = false;
        return;
    }
    s_spam.active = true;
    s_spam.buttons = buttons;
    s_spam.hz = hz;
    s_spam.startTick = NowTick();
    s_spam.endTick = s_spam.startTick + MsToTick((uint64_t)secs * 1000);
}

void StopSpam() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_spam.active = false;
}

void SetBlocked(uint64_t mask) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_blocked = mask & ~nn::hid::Minus;   /* Minus = emergency exit, never blocked */
}
void ClearBlocked() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_blocked = 0;
}

void SetZeroSticks(bool on) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_zeroSticks = on;
}

void BeginBypass() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_bypassDepth == 0) s_bypassTid = CurTid();
    s_bypassDepth++;
}
void EndBypass() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_bypassDepth > 0) s_bypassDepth--;
}

/* Consume latched real-input rising edges matching mask. */
uint64_t ConsumeRealPress(uint64_t mask) {
    std::lock_guard<std::mutex> lk(s_mtx);
    uint64_t r = s_realEdges & mask;
    s_realEdges &= ~mask;
    return r;
}

/* ---------------- pad navigation (merged path only) ---------------- */

void NavUpdate(const nn::hid::NpadHandheldState& st) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_navEdges |= st.buttons & ~s_navPrev;
    s_navPrev = st.buttons;
    s_navHeld = st.buttons;
    s_navLx = st.analogStickL[0];
    s_navLy = st.analogStickL[1];
}

uint64_t NavConsume(uint64_t mask) {
    std::lock_guard<std::mutex> lk(s_mtx);
    uint64_t r = s_navEdges & mask;
    s_navEdges &= ~mask;
    return r;
}

uint64_t NavHeld() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_navHeld;
}

void NavStick(int* x, int* y) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (x) *x = s_navLx;
    if (y) *y = s_navLy;
}

void NavClear() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_navEdges = 0;
    s_navPrev = s_navHeld;
}

void ResetAll() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_blocked = 0;
    s_zeroSticks = false;
    s_tapCount = 0;
    s_spam.active = false;
}

/* src: 0=Handheld hook, 1=FullKey hook, 2=JoyDual hook, 3=merged path
 * (GetMergedNpadState - feeds our own UI: no real-edge latch, no Filter). */
void Apply(nn::hid::NpadHandheldState* st, nn::hid::NpadHandheldState* navOut,
           int src, uint32_t npadId) {
    if (st == nullptr) return;
    uint64_t now = NowTick();
    uint64_t nowMs = now * 1000 / nn::os::GetSystemTickFrequency();
    uint64_t inject = 0;
    uint64_t injectForce = 0;
    uint64_t blocked = 0;
    bool zeroLs = false;
    /* ids 0..7 map to slots 0..7 of this style; anything else (handheld
     * 0x20, player_n_plus, unknown) folds into slot 8 */
    int slot = src * 9 + (npadId <= 7 ? (int)npadId : 8);

    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_bypassDepth > 0 && s_bypassTid == CurTid())
            return;                        /* our own UI poll sees real input */
        /* latch real-input rising edges before masking (one physical
         * press = one edge, whichever sources report it) */
        if (src >= 0 && src <= 2) {
            s_srcReal[slot] = st->buttons;
            s_srcRealMs[slot] = nowMs;
            uint64_t comb = 0;
            for (int i = 0; i < 27; i++)
                if (nowMs - s_srcRealMs[i] <= 250) comb |= s_srcReal[i];
            s_realEdges |= comb & ~s_prevRealComb;
            s_prevRealComb = comb;
        }
        blocked = s_blocked;
        zeroLs = s_zeroSticks;

        /* spam: half-period on/off while active */
        if (s_spam.active && s_spam.hz > 0) {
            if (now >= s_spam.endTick) {
                s_spam.active = false;
            } else {
                uint64_t period = nn::os::GetSystemTickFrequency() / s_spam.hz;
                if (period > 0 && (now - s_spam.startTick) % period < period / 2)
                    inject |= s_spam.buttons;
            }
        }

        /* tap queue: current entry holds bits for hold_ms, then gap_ms off */
        while (s_tapCount > 0) {
            const TapEntry& e = s_taps[s_tapHead];
            uint64_t elapsed = now - s_tapStartTick;
            if (elapsed < MsToTick(e.hold_ms)) {
                if (e.force) injectForce |= e.buttons;
                else         inject |= e.buttons;
                break;
            }
            if (elapsed < MsToTick(e.hold_ms + e.gap_ms))
                break;                       /* release window */
            s_tapHead = (s_tapHead + 1) & 31;
            s_tapCount--;
            s_tapStartTick = now;
        }

        /* nav snapshot: real | inject, before blocking / stick zeroing */
        if (navOut) {
            *navOut = *st;
            navOut->buttons |= inject | injectForce;
        }
    }

    st->buttons |= inject;      /* normal taps are masked like real input */
    st->buttons &= ~blocked;
    st->buttons |= injectForce; /* the lobby's own injects always get through */
    if (zeroLs) {
        st->analogStickL[0] = 0; st->analogStickL[1] = 0;
        st->analogStickR[0] = 0; st->analogStickR[1] = 0;
        st->buttons &= ~0xFF0000ULL;   /* stick-direction button bits too */
    }
    /* menu model sees post-block buttons for the game's own calls only */
    if (src >= 0 && src <= 2)
        menu::Filter(slot, &st->buttons, st->analogStickL, st->analogStickR,
                     nowMs);
}

struct BtnName { const char* name; uint64_t bit; };
static const BtnName kBtns[] = {
    {"A", nn::hid::A}, {"B", nn::hid::B}, {"X", nn::hid::X}, {"Y", nn::hid::Y},
    {"L", nn::hid::L}, {"R", nn::hid::R}, {"ZL", nn::hid::ZL}, {"ZR", nn::hid::ZR},
    {"Plus", nn::hid::Plus}, {"Minus", nn::hid::Minus},
    {"Up", nn::hid::Up}, {"Down", nn::hid::Down},
    {"Left", nn::hid::Left}, {"Right", nn::hid::Right},
    {"LStick", nn::hid::LStick}, {"RStick", nn::hid::RStick},
};

uint64_t MaskFromNames(const char* names) {
    if (names == nullptr) return 0;
    uint64_t mask = 0;
    char buf[96];
    strncpy(buf, names, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* p = strtok(buf, ","); p; p = strtok(nullptr, ",")) {
        while (*p == ' ') p++;
        for (const auto& b : kBtns)
            if (strcmp(p, b.name) == 0) { mask |= b.bit; break; }
    }
    return mask;
}

} // namespace mp::input
