#include "Multiplayer.hpp"
#include "Config.hpp"
#include "imgui/imgui_nvn.h"
#include <string>
#include <cstring>
#include <cstdio>

extern "C" {
    struct MultiplayerState {
        char magic[16];
        uint32_t isGameOpen;
        uint32_t isSongStarted;
        uint32_t currentPvId;
        void* currentScorePtr;
    };

    alignas(0x1000) MultiplayerState g_multiplayerState = {
        "MDVA_STATE_123",
        1, // isGameOpen
        0, // isSongStarted
        0, // currentPvId
        nullptr // currentScorePtr
    };
}

namespace Multiplayer {

    void Init() {
        // Write the memory address of the state struct to the SD card
        // so the Python script can easily find it without scanning memory!
        FILE* f = fopen("ExlSD:/MultiplayerStatePtr.txt", "w");
        if (f) {
            fprintf(f, "%llx", (unsigned long long)&g_multiplayerState);
            fclose(f);
        }
    }

    void OnGameStateStart(int32_t pvId, void* scorePtr) {
        if (!Config::enableMultiplayer) return;
        g_multiplayerState.isSongStarted = 1;
        g_multiplayerState.currentPvId = pvId;
        g_multiplayerState.currentScorePtr = scorePtr;
    }

    void Draw() {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        
        // Increase font size (ImGui scale method)
        ImFont* font = ImGui::GetFont();
        float oldScale = font->Scale;
        font->Scale = 2.0f;
        ImGui::PushFont(font);

        // Simple overlay indicator that we are running
        fg->AddText(ImVec2(12.0f, 12.0f), IM_COL32(0, 255, 0, 255), "Multiplayer Mod Active (sys-botbase)");

        if (g_multiplayerState.isSongStarted) {
            fg->AddText(ImVec2(12.0f, 48.0f), IM_COL32(0, 255, 255, 255), "Song Started!");
        }

        // Restore font size
        ImGui::PopFont();
        font->Scale = oldScale;

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
