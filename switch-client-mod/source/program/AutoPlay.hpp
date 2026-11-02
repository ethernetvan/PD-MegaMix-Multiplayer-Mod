#pragma once
#include <cstdint>
#include <cstddef>

struct PVGameArcade;
struct SoundEffect;
namespace diva_nc { struct vec2; }

namespace AutoPlay {

extern bool g_enabled;
extern int32_t g_hitPct;   /* 0..100: fraction of notes AutoPlay hits */

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
