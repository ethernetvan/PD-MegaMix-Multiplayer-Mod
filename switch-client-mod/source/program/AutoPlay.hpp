#pragma once
#include <cstdint>
#include <cstddef>

struct PVGameArcade;
struct SoundEffect;
namespace diva_nc { struct vec2; }

namespace AutoPlay {

extern bool g_enabled;
extern int32_t g_hitPct;   /* Mode_Custom: fraction of notes to hit, 0..100 */

/* Bot "skill" modes; ZR x3 in song select cycles them (skipping Custom). */
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

void init();
void CheckMenuToggle();
void Update(PVGameArcade* game);
void PostUpdate(PVGameArcade* game);

void ProcessSlideHit(
    PVGameArcade* game,
    int32_t& final_hit_state,
    bool* play_default_se,
    size_t* rating_count,
    diva_nc::vec2* rating_pos,
    SoundEffect* se,
    int32_t* multi_count,
    float* player_hit_time,
    int32_t* target_index,
    bool* is_success_note,
    bool* slide,
    bool* slide_chain,
    bool* slide_chain_start,
    bool* slide_chain_max,
    bool* slide_chain_continues
);

} // namespace AutoPlay
