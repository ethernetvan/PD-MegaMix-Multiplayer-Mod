#pragma once
/* PC AutoPlay: vanilla-MM+ port of switch-client-mod/source/program/
 * AutoPlay.hpp — same modes/API, reimplemented over the game's own
 * PvGameTarget list and GetPvKeyState* hooks (no New Classics). */
#include <cstdint>

namespace diva { struct PVGameArcade; }

namespace AutoPlay {

extern bool g_enabled;
extern int32_t g_hitPct;   /* Mode_Custom: fraction of notes to hit, 0..100 */

enum Mode {
    Mode_Custom = 0,   /* g_hitPct-driven (admin autoplay --pct) */
    Mode_Auto,         /* 100 %: every note Cool */
    Mode_Pro,          /* ~97 %: ~1/32 notes get a degraded judgment, rare miss */
    Mode_97,           /* ~97 %: deterministic miss every 32nd note */
    Mode_Streak,       /* ~94 % bursty: drops pairs every 32 notes */
    Mode_Human,        /* hits (nearly) all, mixed Cool/Fine/Safe judgments */
    Mode_Clutch,       /* weak start -> perfect finish (scripted comeback) */
    Mode_Tie,          /* fixed judgment pattern: identical score everywhere */
    Mode_Count
};
extern int32_t g_mode;

const char* ModeTag();                  /* gold-pill label */
const char* ModeName();                 /* lowercase name for lobby status */
int32_t     EffectivePct();             /* nominal hit rate for lobby status */
int32_t     ModeFromName(const char*);  /* admin "mode" -> enum, -1 unknown */

/* in-song indicator: stamped each PV-game-task frame so the badge knows
 * a song (incl. pause) is actually running */
void OnGameTaskFrame();
bool IndicatorVisible();
void DrawIndicator();

/* frame planning, called from the arcade-ctrl entry mid-hook (before the
 * frame's GetPvKeyState* reads); song teardown, from TaskPvGameDest */
void Update(diva::PVGameArcade* game);
void OnGameEnd();

void init();   /* installs the input + ctrl-entry hooks */

} // namespace AutoPlay
