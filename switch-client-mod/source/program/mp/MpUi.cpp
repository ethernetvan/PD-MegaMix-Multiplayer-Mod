#include "MpLobby.hpp"
#include "MpNet.hpp"
#include "../Config.hpp"
#include "../overlays/VirtualKeyboard.hpp"
#include "imgui/imgui.h"
#include "../ImGui.hpp"
#include <cstdio>
#include <cstring>

namespace mp::lobby {

/* This NVN ImGui backend clips window content incorrectly unless a
 * full-screen clip rect is pushed (same pattern as FpsOverlay etc.). */
static bool BeginWin(const char* id, bool* open, ImGuiWindowFlags flags) {
    bool r = ImGui::Begin(id, open, flags);
    ImGui::GetWindowDrawList()->PushClipRectFullScreen();
    return r;
}
static void EndWin() {
    ImGui::GetWindowDrawList()->PopClipRect();
    ImGui::End();
}

static char s_nameBuf[64] = "";
static char s_codeBuf[8] = "";

static void SubmitName() {
    if (s_nameBuf[0]) CmdSetName(s_nameBuf);
}
static void SubmitCode() {
    if (s_codeBuf[0]) CmdLobbyJoin(s_codeBuf);
    s_codeBuf[0] = 0;
}

static void OpenNameKb() {
    strncpy(s_nameBuf, Config::mpPlayerName.c_str(), sizeof(s_nameBuf) - 1);
    VirtualKeyboard::Open(s_nameBuf, sizeof(s_nameBuf), "SET NAME", SubmitName);
}
static void OpenCodeKb() {
    s_codeBuf[0] = 0;
    VirtualKeyboard::Open(s_codeBuf, sizeof(s_codeBuf), "JOIN", SubmitCode);
}

void DrawPanel() {
    Snapshot s;
    GetSnapshot(&s);
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImGui::SetNextWindowPos(ImVec2(20 * k, 90 * k), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(310 * k, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    bool open = true;
    if (!BeginWin("##mplobby", &open,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_AlwaysAutoResize)) {
        EndWin();
        return;
    }
    ImGui::g_imguiHasFocus = true;

    if (!InLobby()) {
        ImGui::Text("Name: %s", Config::mpPlayerName.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit")) OpenNameKb();
        if (ImGui::Button("Create Lobby")) CmdLobbyCreate();
        ImGui::Text("Code: %s", s_codeBuf);
        ImGui::SameLine();
        if (ImGui::SmallButton("Join...")) OpenCodeKb();
        EndWin();
        return;
    }

    ImGui::Text("Lobby %s", s.code);
    ImGui::SameLine();
    if (ImGui::SmallButton("Leave")) CmdLobbyLeave();
    ImGui::Separator();
    for (int i = 0; i < s.memberCount; i++) {
        const auto& m = s.members[i];
        ImVec4 c = m.connected ? ImVec4(0.4f, 1.0f, 0.4f, 1)
                               : ImVec4(0.5f, 0.5f, 0.5f, 1);
        ImGui::TextColored(c, "%s%s%s%s", m.is_leader ? "[L] " : "",
                           m.name, strcmp(m.cid, s.myCid) == 0 ? " (you)" : "",
                           m.ready ? "" : "");
        if (!m.ready && m.reason[0]) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", m.reason);
        }
        if (m.role[0] && strcmp(m.role, "player") != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", m.role);
        }
    }
    if (s.statusLine[0]) ImGui::TextDisabled("%s", s.statusLine);
    if (s.notReady[0])
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", s.notReady);
    if (s.phase == 1 /*L_SELECT*/ && strcmp(s.leaderCid, s.myCid) == 0 &&
        s.notReady[0])
        ImGui::TextDisabled("Press A to start anyway");
    EndWin();
}

void DrawLeaderboard() {
    Snapshot s;
    GetSnapshot(&s);
    if (s.memberCount == 0) return;
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x - 12 * k - 200 * k, 95 * k),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200 * k, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    BeginWin("##mpboard", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextDisabled("MP Live");
    for (int r = 0; r < s.boardCount; r++) {
        const auto& b = s.board[r];
        ImVec4 c = b.me ? ImVec4(1.0f, 0.9f, 0.3f, 1)
                 : (b.connected ? ImVec4(1, 1, 1, 1)
                                : ImVec4(0.5f, 0.5f, 0.5f, 1));
        ImGui::TextColored(c, "%d. %-12.12s %d", r + 1, b.name, b.score);
    }
    EndWin();
}

void DrawStandings() {
    Snapshot s;
    GetSnapshot(&s);
    if (s.standingCount == 0) return;
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x - 12 * k - 260 * k, 140 * k),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260 * k, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    BeginWin("##mpstand", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Round standings");
    for (int i = 0; i < s.standingCount; i++) {
        const auto& r = s.standing[i];
        ImVec4 c = r.me ? ImVec4(1.0f, 0.9f, 0.3f, 1) : ImVec4(1, 1, 1, 1);
        ImGui::TextColored(c, "%d. %-10.10s %d  %.1f%%",
                           r.rank, r.name, r.score, (double)r.pct);
    }
    if (!s.standingsComplete)
        ImGui::TextDisabled("Waiting for player(s)...");
    EndWin();
}

void DrawWaiting() {
    Snapshot s;
    GetSnapshot(&s);
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f - 240 * k, ds.y * 0.35f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(480 * k, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    BeginWin("##mpwait", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Waiting for players... %d/%d loaded", s.loadedCount,
                s.playerCount);
    ImGui::TextDisabled("don't unpause - the game resumes automatically");
    EndWin();
}

void DrawBanners() {
    Snapshot s;
    GetSnapshot(&s);
    if (s.bannerCount == 0) return;
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    for (int i = 0; i < s.bannerCount; i++) {
        float a = s.banners[i].ttl < 0.5f ? s.banners[i].ttl / 0.5f : 1.0f;
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f - 200 * k,
                                       (40 + i * 30) * k), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.5f * a);
        char id[16]; snprintf(id, sizeof(id), "##mpb%d", i);
        BeginWin(id, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextColored(ImVec4(1, 1, 1, a), "%s", s.banners[i].text);
        EndWin();
    }
}

} // namespace mp::lobby
