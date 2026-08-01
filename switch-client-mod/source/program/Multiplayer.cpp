#include "Multiplayer.hpp"
#include "Config.hpp"
#include "imgui/imgui_nvn.h"

#include <string>

namespace Multiplayer {

    static bool g_isConnected = false;
    static bool g_sendSongStarted = false;

    void Init() {
        // Mock connection for now
        g_isConnected = true; 
    }

    void OnGameStateStart() {
        g_sendSongStarted = true;
    }

    void Draw() {
        // Use the foreground draw list — draws on top of everything,
        // no ImGui window needed, no clipping issues.
        ImDrawList* fg = ImGui::GetForegroundDrawList();

        // === PINK DIAGONAL LINE across the whole screen ===
        fg->AddLine(
            ImVec2(0.0f, 0.0f),
            ImVec2(SCREEN_W, SCREEN_H),
            IM_COL32(255, 105, 180, 255),  // hot pink
            3.0f                           // thickness
        );

        // === TOP LEFT: connection status text ===
        const char* statusText = g_isConnected ? "Connected to PC" : "Not Connected";
        ImU32 statusColor = g_isConnected ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 0, 0, 255);
        fg->AddText(ImVec2(12.0f, 12.0f), statusColor, statusText);

        if (g_isConnected && g_sendSongStarted) {
            fg->AddText(ImVec2(12.0f, 28.0f), IM_COL32(0, 255, 255, 255), "Song Started!");
        }

        // === BOTTOM RIGHT: 40x40 white box ===
        float bx = SCREEN_W - 50.0f;
        float by = SCREEN_H - 50.0f;
        fg->AddRectFilled(
            ImVec2(bx, by),
            ImVec2(bx + 40.0f, by + 40.0f),
            IM_COL32(255, 255, 255, 255)
        );
    }
}
