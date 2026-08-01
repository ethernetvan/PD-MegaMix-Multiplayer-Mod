#include "Multiplayer.hpp"
#include "Config.hpp"
#include "imgui/imgui_nvn.h"

#include <thread>
#include <atomic>
#include <string>
#include <chrono>
#include <cstring>

// BSD Sockets
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace Multiplayer {

    static std::atomic<bool> g_isConnected{false};
    static std::atomic<bool> g_sendSongStarted{false};
    static int g_socket = -1;

    void ConnectionThread() {
        while (true) {
            if (!Config::enableMultiplayer) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            if (!g_isConnected) {
                if (g_socket != -1) {
                    close(g_socket);
                    g_socket = -1;
                }

                g_socket = socket(AF_INET, SOCK_STREAM, 0);
                if (g_socket >= 0) {
                    struct sockaddr_in serverAddr;
                    std::memset(&serverAddr, 0, sizeof(serverAddr));
                    serverAddr.sin_family = AF_INET;
                    serverAddr.sin_port = htons(Config::serverPort);
                    inet_pton(AF_INET, Config::serverIp.c_str(), &serverAddr.sin_addr);

                    if (connect(g_socket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == 0) {
                        g_isConnected = true;
                    } else {
                        close(g_socket);
                        g_socket = -1;
                    }
                }
                
                if (!g_isConnected) {
                    std::this_thread::sleep_for(std::chrono::seconds(3)); // retry delay
                }
            } else {
                // Connection is established. Check for data to send.
                if (g_sendSongStarted) {
                    g_sendSongStarted = false;
                    const char* msg = "SONG_STARTED\n";
                    int bytesSent = send(g_socket, msg, std::strlen(msg), 0);
                    if (bytesSent < 0) {
                        g_isConnected = false; // Disconnected
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // polling delay
            }
        }
    }

    void Init() {
        if (!Config::enableMultiplayer) return;
        std::thread(ConnectionThread).detach();
    }

    void OnGameStateStart() {
        if (!Config::enableMultiplayer || !g_isConnected) return;
        g_sendSongStarted = true;
    }

    void Draw() {
        if (!Config::enableMultiplayer) return;

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoInputs;

        // Top Left text
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        if (ImGui::Begin("MultiplayerStatusOverlay", nullptr, flags)) {
            if (g_isConnected) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected to PC");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Not Connected");
            }
        }
        ImGui::End();

        // White box in bottom right (40x40)
        ImVec2 screenSize = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(screenSize.x - 50.0f, screenSize.y - 50.0f), ImGuiCond_Always);
        if (ImGui::Begin("MultiplayerBoxOverlay", nullptr, flags)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            draw->AddRectFilled(pos, ImVec2(pos.x + 40.0f, pos.y + 40.0f), IM_COL32(255, 255, 255, 255));
            ImGui::Dummy(ImVec2(40.0f, 40.0f)); // reserve space so the window bounds wrap it
        }
        ImGui::End();
    }
}
