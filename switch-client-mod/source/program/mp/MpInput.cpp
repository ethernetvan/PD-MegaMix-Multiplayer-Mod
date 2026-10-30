#include "MpInput.hpp"
#include <nn.hpp>
#include <mutex>
#include <cstring>

namespace mp::input {

struct TapEntry {
    uint64_t buttons;
    uint32_t hold_ms;
    uint32_t gap_ms;
};

static TapEntry    s_taps[32];
static uint32_t    s_tapHead = 0, s_tapCount = 0;
static uint64_t    s_tapStartTick = 0;
static std::mutex  s_mtx;
static uint64_t    s_blocked = 0;
static bool        s_zeroLStick = false;
static int         s_bypassDepth = 0;   /* set while our own UI polls pad */
static uint64_t    s_bypassTid = 0;

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

void Tap(uint64_t buttons, uint32_t hold_ms, uint32_t gap_ms) {
    if (buttons == 0) return;
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_tapCount == 32) {                  /* full: drop oldest */
        s_tapHead = (s_tapHead + 1) & 31;
        s_tapCount--;
    }
    uint32_t idx = (s_tapHead + s_tapCount) & 31;
    s_taps[idx] = {buttons, hold_ms, gap_ms};
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

void SetZeroLStick(bool on) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_zeroLStick = on;
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

void ResetAll() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_blocked = 0;
    s_zeroLStick = false;
    s_tapCount = 0;
    s_spam.active = false;
}

void Apply(nn::hid::NpadHandheldState* st) {
    if (st == nullptr) return;
    uint64_t now = NowTick();
    uint64_t inject = 0;
    uint64_t blocked = 0;
    bool zeroLs = false;

    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_bypassDepth > 0 && s_bypassTid == CurTid())
            return;                        /* our own UI poll sees real input */
        blocked = s_blocked;
        zeroLs = s_zeroLStick;

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
                inject |= e.buttons;
                break;
            }
            if (elapsed < MsToTick(e.hold_ms + e.gap_ms))
                break;                       /* release window */
            s_tapHead = (s_tapHead + 1) & 31;
            s_tapCount--;
            s_tapStartTick = now;
        }
    }

    st->buttons &= ~blocked;
    st->buttons |= inject;
    if (zeroLs) { st->analogStickL[0] = 0; st->analogStickL[1] = 0; }
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
