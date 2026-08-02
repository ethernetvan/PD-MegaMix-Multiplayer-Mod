#include "Multiplayer.hpp"
#include "Config.hpp"
#include "imgui/imgui_nvn.h"

#include "nn/os/os_thread_api.hpp"
#include "nn/os/os_condition_variable_common.hpp"
#include "nn/os/os_thread_type.hpp"
#include "nn/time/time_timespan.hpp"

#include <string>
#include <atomic>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

extern "C" {
    unsigned int smInitialize(void);
    void smExit(void);
    unsigned int socketInitializeDefault(void);
    void socketExit(void);
}

namespace Multiplayer {

    static std::atomic<bool> g_isConnected{false};
    static std::atomic<bool> g_sendSongStarted{false};
    // static int g_socket = -1;

    static nn::os::ThreadType s_networkThread;
    alignas(0x1000) static uint8_t s_networkThreadStack[0x4000];

    void NetworkThreadFunc(void*) {
        // smInitialize() and socketInitializeDefault() CRASH on boot because 
        // the Switch OS prohibits plugins from acquiring a second sm:m session 
        // when the game already has one active. 
        // We revert to a mock loop so the game boots.
        
        while (true) {
            if (!Config::enableMultiplayer) {
                nn::os::SleepThread(nn::TimeSpan::FromSeconds(1));
                continue;
            }

            if (!g_isConnected) {
                // Mock connection success
                g_isConnected = true;
            } else {
                if (g_sendSongStarted) {
                    g_sendSongStarted = false;
                    // Mock send success
                }

                nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(100)); // polling delay
            }
        }
    }

    void Init() {
        if (!Config::enableMultiplayer) return;
        
        nn::os::CreateThread(&s_networkThread, NetworkThreadFunc, nullptr, s_networkThreadStack, sizeof(s_networkThreadStack), 16, nn::os::IdealCoreUseDefault);
        nn::os::StartThread(&s_networkThread);
    }

    void OnGameStateStart() {
        if (!Config::enableMultiplayer || !g_isConnected) return;
        g_sendSongStarted = true;
    }

    void Draw() {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        
        // Increase font size (ImGui scale method)
        ImFont* font = ImGui::GetFont();
        float oldScale = font->Scale;
        font->Scale = 2.0f;
        ImGui::PushFont(font);

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
