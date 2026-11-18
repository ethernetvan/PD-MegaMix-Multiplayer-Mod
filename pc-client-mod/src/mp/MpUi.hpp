#pragma once

/* ImGui draws for the MP lobby — declared in MpLobby.hpp (same namespace):
 * DrawPanel, DrawBanners, DrawLeaderboard, DrawWaiting, PanelWanted. */
#include "MpLobby.hpp"

struct ImDrawList;
namespace mp::lobby {
/* HUD (parity) windows created via BeginWin since the last reset */
void HudCaptureReset();
int  HudCaptured(ImDrawList** out, int max);
}
