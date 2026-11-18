#include "MpMenu.hpp"
#include "MpInput.hpp"
#include "../platform/NxShim.hpp"
#include <mutex>

namespace mp::menu {

/* holds: how long a page ignores edges after it (or its last accepted
 * nav) appeared; nav: gap between accepted Up/Down edges. FRAME counts
 * (60-fps equivalents of the old ms values): wall-clock holds broke on
 * 26-46 FPS emulators because the results-page input locks are
 * frame-based animations. s_frame ticks once per presented frame
 * (mp::OnFrame -> menu::Frame). Results values = what passed 6
 * consecutive 3-emulator auto-return rounds (90/60/100/45/15) + 6, except
 * VP: those rounds were all fails, and a CLEARED result's VP page (count-up)
 * locks input longer - a leader's A at ~106 frames was eaten once - so VP
 * keeps the old 120 until a real "VP done" signal is found. */
static uint64_t RES_INIT_HOLD       = 96;
static uint64_t RES_SCORE_A_HOLD    = 66;
static uint64_t RES_VP_HOLD         = 120;
static uint64_t RES_MENU_HOLD       = 51;
static uint64_t RES_NAV_HOLD        = 21;
static const uint64_t PAUSE_OPEN_HOLD     = 21;
static const uint64_t PAUSE_NAV_HOLD      = 9;
static const uint64_t PAUSE_QUIT_TIMEOUT  = 3000;   /* wall-clock ms */

/* a source is "live" for the held-OR if Apply saw it this recently */
static const uint64_t SRC_WINDOW = 250;

using nn::hid::A;    using nn::hid::B;    using nn::hid::Up;
using nn::hid::Down; using nn::hid::Plus; using nn::hid::Minus;

static const uint64_t RES_ALLOW   = A | Up | Down | Minus;
static const uint64_t PAUSE_ALLOW = A | B | Up | Down | Plus | Minus;

static std::mutex  s_mtx;
static Page        s_page = P_NONE;
static int         s_cursor = 0;
static bool        s_guard = false;
static uint64_t    s_frame = 0;
static uint64_t    s_readyFrame = 0;
static uint64_t    s_pageFrame = 0;
static uint64_t    s_pageSince = 0;
static uint32_t    s_events = 0;
static uint64_t    s_suppress = 0;   /* buttons stripped until released */
static uint64_t    s_srcHeld[27] = {};  /* per (style, controller slot) */
static uint64_t    s_srcMs[27] = {};
static uint64_t    s_prevCombined = 0;

void Frame() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_frame++;
}

static void SetPage(Page p, uint64_t now, uint64_t holdFrames) {
    s_page = p;
    s_pageSince = now;
    s_pageFrame = s_frame;
    s_readyFrame = s_frame + holdFrames;
}

void BeginResults(bool guard, uint64_t now) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_guard = guard;
    s_cursor = 0;
    SetPage(P_RES_SCORE, now, RES_INIT_HOLD);
}

void SetResHolds(uint64_t init, uint64_t scoreA, uint64_t vp,
                 uint64_t menu, uint64_t nav) {
    std::lock_guard<std::mutex> lk(s_mtx);
    /* admin `menu_holds` still passes ms; convert to 60-fps frames */
    auto ms2fr = [](uint64_t ms) { return (ms * 60 + 500) / 1000; };
    if ((int64_t)init   >= 0) RES_INIT_HOLD    = ms2fr(init);
    if ((int64_t)scoreA >= 0) RES_SCORE_A_HOLD = ms2fr(scoreA);
    if ((int64_t)vp     >= 0) RES_VP_HOLD      = ms2fr(vp);
    if ((int64_t)menu   >= 0) RES_MENU_HOLD    = ms2fr(menu);
    if ((int64_t)nav    >= 0) RES_NAV_HOLD     = ms2fr(nav);
}

void ResultsVpShown(uint64_t now) {
    std::lock_guard<std::mutex> lk(s_mtx);
    /* only the score page advances: a re-fired layer while the menu is up
     * must not reset the model (the next A would pass on Retry) */
    if (s_page != P_RES_SCORE) return;
    s_cursor = 0;
    SetPage(P_RES_VP, now, RES_VP_HOLD);
}

void BeginPause(bool guard, uint64_t now) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_guard = guard;
    s_cursor = 0;
    SetPage(P_PAUSE, now, PAUSE_OPEN_HOLD);
}

void SetGuard(bool guard) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_guard = guard;
}

void ExitFallback(uint64_t now) {
    std::lock_guard<std::mutex> lk(s_mtx);
    /* P1: B on the results menu does NOT close it back to win_vp -
     * go straight to MENU on Return and let the next A leave; if the
     * real cursor was on Retry the lobby's retry recovery catches it. */
    s_cursor = 1;
    SetPage(P_RES_MENU, now, 0);
}

void End() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_page = P_NONE;
    /* s_suppress is kept: suppressed buttons stay stripped until the
     * user releases them everywhere (s_suppress &= combined in Filter). */
}

void ForceCursor(int c) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_cursor = c;
}

uint32_t ConsumeEvents() {
    std::lock_guard<std::mutex> lk(s_mtx);
    uint32_t e = s_events;
    s_events = 0;
    return e;
}

Status Get(uint64_t now) {
    std::lock_guard<std::mutex> lk(s_mtx);
    Status s{};
    s.page = (int)s_page;
    s.cursor = s_cursor;
    s.ready = s_page != P_NONE && s_frame >= s_readyFrame;
    s.guard = s_guard;
    s.pageSince = s_pageSince;
    s.pageFrames = s_frame - s_pageFrame;
    return s;
}

void Filter(int slot, uint64_t* buttons, int32_t* stickL, int32_t* stickR,
            uint64_t now) {
    if (buttons == nullptr || slot < 0 || slot > 26) return;
    std::lock_guard<std::mutex> lk(s_mtx);

    s_srcHeld[slot] = *buttons;
    s_srcMs[slot] = now;
    uint64_t held = 0;
    for (int i = 0; i < 27; i++)
        if (now - s_srcMs[i] <= SRC_WINDOW) held |= s_srcHeld[i];
    uint64_t edges = held & ~s_prevCombined;
    s_prevCombined = held;
    s_suppress &= held;

    if (s_page == P_NONE) {
        *buttons &= ~s_suppress;
        return;
    }

    /* tracked page: kill the sticks and every button the menu can't use */
    if (stickL) { stickL[0] = 0; stickL[1] = 0; }
    if (stickR) { stickR[0] = 0; stickR[1] = 0; }
    uint64_t allow = (s_page == P_PAUSE || s_page == P_PAUSE_QUIT)
        ? PAUSE_ALLOW : RES_ALLOW;
    *buttons &= allow & ~s_suppress;

    /* real pause-menu cursor (captured task, +0x278): overrides the model's
     * counted cursor whenever the menu task is live. The veto itself stays
     * swallow-at-confirm (Switch semantics): the game's own cursor and
     * highlight stay in sync, the A edge on a vetoed item is stripped. */
    if (s_page == P_PAUSE) {
        int rc = mp::input::PauseMenuCursor();
        if (rc >= 0) s_cursor = rc;
    }

    for (uint64_t e : {A, B, Up, Down}) {
        if (!(edges & e)) continue;
        if (s_frame < s_readyFrame) {    /* too early: keep both in agreement */
            s_suppress |= e;
            *buttons &= ~e;
            continue;
        }
        bool pass = false;
        switch (s_page) {
        case P_RES_SCORE:
            if (e == A) { pass = true; s_readyFrame = s_frame + RES_SCORE_A_HOLD; }
            break;
        case P_RES_VP:
            if (e == A) { pass = true; SetPage(P_RES_MENU, now, RES_MENU_HOLD); s_cursor = 0; }
            break;
        case P_RES_MENU:
            if (e == Up || e == Down) {
                pass = true;
                s_cursor ^= 1;
                s_readyFrame = s_frame + RES_NAV_HOLD;
            } else if (e == A) {
                if (s_cursor == 0 && s_guard) {
                    s_events |= EV_RETRY_BLOCKED;
                } else {
                    pass = true;
                    SetPage(P_RES_EXIT, now, 0);
                }
            }
            break;
        case P_PAUSE:
            if (e == Up) {
                pass = true;
                s_cursor = (s_cursor + 4) % 5;
                s_readyFrame = s_frame + PAUSE_NAV_HOLD;
            } else if (e == Down) {
                pass = true;
                s_cursor = (s_cursor + 1) % 5;
                s_readyFrame = s_frame + PAUSE_NAV_HOLD;
            } else if (e == A) {
                if (s_cursor == 0)      s_events |= EV_RESUME;
                else if (s_cursor == 1) s_events |= EV_RETRY_BLOCKED;
                else if (s_cursor == 4) {
                    pass = true;
                    s_events |= EV_QUIT;
                    SetPage(P_PAUSE_QUIT, now, 0);
                } else                  s_events |= EV_ITEM_BLOCKED;
            } else if (e == B) {
                s_events |= EV_RESUME;
            }
            break;
        case P_PAUSE_QUIT:
        case P_RES_EXIT:
        default:
            break;                     /* everything is swallowed */
        }
        if (!pass) {                   /* swallowed edge stays stripped
                                        * until released everywhere */
            s_suppress |= e;
            *buttons &= ~e;
        }
    }

    /* PAUSE_QUIT timeout also runs when no edges arrived */
    if (s_page == P_PAUSE_QUIT && now - s_pageSince >= PAUSE_QUIT_TIMEOUT) {
        s_cursor = 4;
        SetPage(P_PAUSE, now, 0);
    }
}

} // namespace mp::menu
