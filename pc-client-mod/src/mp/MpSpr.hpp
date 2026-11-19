#pragma once
/* put_sprite hook: the multiplayer race lead fill is drawn through the
 * game's own sprite queue so it lands UNDER the gauge's BORDER/G/E markers
 * (ImGui draws after the whole scene and can only ever sit on top).
 * MpUi publishes the per-player lead segments once per ImGui frame; the
 * hook reads that lock-free copy on the game's sprite thread. */
#include <cstdint>

namespace mp::spr {

void Install();

/* Render-thread side (called by DrawGaugeRace): the displayed lead data.
 * pct[i] is a smoothed 0..100 clear % of a player ahead of us (farthest
 * first), color[i] the BGRA tint (alpha honoured). n=0 disables. */
void RaceFillPublish(float myPct, int n, const float* pct,
                     const uint32_t* color);

#ifdef MP_DEV_TOOLS
/* dbg_cmd sprtrace {n}: capture the next n put_sprite submissions and dump
 * them to mp_log.txt (kind/id/index/layer/prio/rect/uv/color/caller). */
void TraceArm(int n);
#endif

} // namespace mp::spr
