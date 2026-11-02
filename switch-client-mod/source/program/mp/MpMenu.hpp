#pragma once

#include <cstdint>

namespace mp::menu {

/* Blind model of the results + pause menus. The game exposes no signal for
 * the cursor in either, so we track pages/cursor ourselves and strip input
 * the model says can't land, keeping the game and the model in agreement.
 * Fed from mp::input::Apply (src 0..2, post-block buttons) on every
 * GetNpadState hook call. Owns its own mutex; never calls into input:: or
 * the lobby while holding it. */

enum Page {
    P_NONE,
    P_RES_SCORE,   /* first results page (score) */
    P_RES_VP,      /* win_vp page */
    P_RES_MENU,    /* 2-item menu: 0=Retry, 1=Return to Song List */
    P_RES_EXIT,    /* A was passed on the menu - waiting for dest/retry */
    P_PAUSE,       /* pause menu: 0=Resume 1=Retry 2=Lag 3=Options 4=Quit */
    P_PAUSE_QUIT,  /* Return to Song List confirmed, sel pending */
};

enum Ev : uint32_t {
    EV_RESUME        = 1,   /* user hit Resume (A@c0 or B) in the pause menu */
    EV_RETRY_BLOCKED = 2,   /* Retry swallowed (results or pause) */
    EV_ITEM_BLOCKED  = 4,   /* Lag Config/Options swallowed in the pause menu */
    EV_QUIT          = 8,   /* user confirmed Return to Song List from pause */
};

void Frame();                                 /* once per presented frame */
void BeginResults(bool guard, uint64_t now);  /* page SCORE */
void SetResHolds(uint64_t init, uint64_t scoreA, uint64_t vp,
                 uint64_t menu, uint64_t nav);
void ResultsVpShown(uint64_t now);            /* ignored when P_NONE */
void BeginPause(bool guard, uint64_t now);    /* page PAUSE, cursor 0 */
void SetGuard(bool guard);
void ExitFallback(uint64_t now);              /* EXIT timeout: MENU on Return */
void End();                                   /* page NONE */
void ForceCursor(int c);                      /* debug (dbg_cmd menu_cursor) */

/* Post-block buttons + sticks from input::Apply; slot = src*9 + npadId
 * (0..26, per style x controller slot). */
void Filter(int slot, uint64_t* buttons, int32_t* stickL, int32_t* stickR,
            uint64_t now);

uint32_t ConsumeEvents();

struct Status {
    int      page;
    int      cursor;
    bool     ready;       /* s_frame >= readyFrame: next edge is modelled */
    bool     guard;
    uint64_t pageSince;   /* ms when the current page was entered */
    uint64_t pageFrames;  /* frames since the current page was entered */
};
Status Get(uint64_t now);

} // namespace mp::menu
