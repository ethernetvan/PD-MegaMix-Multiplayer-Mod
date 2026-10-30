#include "Multiplayer.hpp"
#include "Config.hpp"
#include "imgui/imgui_nvn.h"
#include <string>
#include <cstring>
#include <cstdio>
#include <vector>
#include "lib.hpp"

extern "C" {
    struct MultiplayerState {
        char magic[16];
        uint32_t isGameOpen;
        uint32_t isSongStarted;
        uint32_t currentPvId;
        void* currentScorePtr;
        
        uint32_t scanTarget;
        uint32_t scanReset;
        int32_t matchCount;
        uint64_t matches[1000];
    };

    alignas(0x1000) MultiplayerState g_multiplayerState = {
        {0}, // magic will be set dynamically
        1, // isGameOpen
        0, // isSongStarted
        0, // currentPvId
        nullptr, // currentScorePtr
        0, // scanTarget
        0, // scanReset
        0, // matchCount
        {0} // matches
    };
}

namespace Multiplayer {

    void Init() {
        std::strcpy(g_multiplayerState.magic, "MDVA_STATE_123");
        
        // Write the memory address of the state struct to the SD card
        // so the Python script can easily find it without scanning memory!
        FILE* f = fopen("ExlSD:/MultiplayerStatePtr.txt", "w");
        if (f) {
            fprintf(f, "%llx", (unsigned long long)&g_multiplayerState);
            fclose(f);
        }
    }

    static std::vector<uint64_t> s_scanMatches;

    void DoMemoryScan(uint32_t target, bool reset) {
        if (reset) {
            s_scanMatches.clear();
            
            // Full Scan - ALL Readable & Writable Memory!
            uint64_t current_addr = 0;
            MemoryInfo meminfo;
            uint32_t pageinfo;
            
            while (R_SUCCEEDED(svcQueryMemory(&meminfo, &pageinfo, current_addr))) {
                if ((meminfo.perm & (Perm_R | Perm_W)) == (Perm_R | Perm_W) && meminfo.type != 0) {
                    uint32_t* ptr = (uint32_t*)meminfo.addr;
                    size_t count = meminfo.size / 4;
                    for (size_t i = 0; i < count; i++) {
                        if (ptr[i] == target) {
                            if ((uint64_t)&ptr[i] == (uint64_t)&g_multiplayerState.scanTarget) continue;
                            s_scanMatches.push_back((uint64_t)&ptr[i]);
                        }
                    }
                }
                current_addr = meminfo.addr + meminfo.size;
                if (current_addr == 0) break;
            }
        } else {
            // Filter existing matches
            std::vector<uint64_t> newMatches;
            for (uint64_t match_addr : s_scanMatches) {
                uint32_t* ptr = (uint32_t*)match_addr;
                
                MemoryInfo meminfo;
                uint32_t pageinfo;
                if (R_SUCCEEDED(svcQueryMemory(&meminfo, &pageinfo, match_addr))) {
                    if ((meminfo.perm & Perm_R) && *ptr == target) {
                        newMatches.push_back(match_addr);
                    }
                }
            }
            s_scanMatches = std::move(newMatches);
        }
        
        // Write back results
        g_multiplayerState.matchCount = s_scanMatches.size();
        size_t copyCount = s_scanMatches.size() > 1000 ? 1000 : s_scanMatches.size();
        for (size_t i = 0; i < copyCount; i++) {
            g_multiplayerState.matches[i] = s_scanMatches[i];
        }
        
        g_multiplayerState.scanTarget = 0;
        g_multiplayerState.scanReset = 0;
    }

    void OnGameStateStart(int32_t pvId, void* scorePtr) {
        if (!Config::enableMultiplayer) return;
        g_multiplayerState.currentPvId = pvId;
        g_multiplayerState.currentScorePtr = scorePtr;
    }

    void SetSongStarted(bool started) {
        if (!Config::enableMultiplayer) return;
        g_multiplayerState.isSongStarted = started ? 1 : 0;
    }

    void Draw() {
        if (g_multiplayerState.scanTarget != 0) {
            DoMemoryScan(g_multiplayerState.scanTarget, g_multiplayerState.scanReset != 0);
        }
        
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
