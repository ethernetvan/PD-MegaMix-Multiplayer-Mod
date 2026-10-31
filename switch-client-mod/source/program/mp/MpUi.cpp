#include "MpLobby.hpp"
#include "MpInput.hpp"
#include "../Config.hpp"
#include "../keyboard.hpp"
#include "../overlays/VirtualKeyboard.hpp"

namespace ImGui { extern bool g_imguiHasFocus; }
#include "imgui/imgui.h"
#include <cstring>
#include <cstdio>
#include <cmath>

namespace mp::lobby {

static bool BeginWin(const char* id, bool* open, ImGuiWindowFlags flags) {
    bool r = ImGui::Begin(id, open, flags);
    ImGui::GetWindowDrawList()->PushClipRectFullScreen();
    return r;
}
static void EndWin() {
    ImGui::GetWindowDrawList()->PopClipRect();
    ImGui::End();
}

static char s_codeBuf[8] = "";
static char s_nameBuf[40] = "";
static int  s_pickIdx = -1;

static void OpenNameKb() {
    strncpy(s_nameBuf, Config::mpPlayerName.c_str(), sizeof(s_nameBuf) - 1);
    VirtualKeyboard::Open(s_nameBuf, sizeof(s_nameBuf), "Player name",
        []() { if (s_nameBuf[0]) CmdSetName(s_nameBuf); });
}
static void OpenCodeKb() {
    VirtualKeyboard::Open(s_codeBuf, sizeof(s_codeBuf), "Lobby code",
        nullptr);
}

/* status text for a member row */
static const char* MemberStatus(const MemberInfo& m) {
    if (!m.connected) return "disconnected";
    if (m.ready) return "ready";
    if (strcmp(m.reason, "modules") == 0) return "choosing modules";
    if (strcmp(m.reason, "wrong_song") == 0) return "wrong song";
    if (strcmp(m.reason, "difficulty") == 0) return "difficulty";
    if (strcmp(m.reason, "not_in_sel") == 0) return "not in song select";
    return "not ready";
}

/* ================================================================
 * Song-select MP button / compact chip / big overlay
 * ============================================================== */

static ImVec2 BtnPos() {
    /* where the old AutoPlay badge sat: top-right, under the score box */
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    return ImVec2(ds.x - 232 * k, 44 * k);
}

void DrawPanel() {
    Snapshot s;
    GetSnapshot(&s);
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImVec2 p = BtnPos();

    if (OverlayOpen()) {
        /* ---- big overlay (~80% of screen) ---- */
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImVec2 sz(ds.x * 0.8f, ds.y * 0.8f);
        ImGui::SetNextWindowPos(
            ImVec2((ds.x - sz.x) * 0.5f, (ds.y - sz.y) * 0.5f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.92f);
        bool open = true;
        if (!BeginWin("Multiplayer##mpov", &open,
                      ImGuiWindowFlags_NoCollapse)) {
            EndWin();
            return;
        }
        ImGui::g_imguiHasFocus = true;
        if (!open) { SetOverlayOpen(false); EndWin(); return; }

        ImGui::Text("Multiplayer");
        ImGui::SameLine(ImGui::GetWindowWidth() - 76 * k);
        if (ImGui::Button("X##close", ImVec2(64 * k, 40 * k)))
            SetOverlayOpen(false);
        ImGui::Separator();

        ImGui::Text("Name: %s", Config::mpPlayerName.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Edit##nm")) OpenNameKb();

        if (!s.connected)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "MP: offline");

        if (!InLobby()) {
            ImVec2 big(sz.x / 3.1f, 96 * k);
            if (ImGui::Button("EDIT NAME", big)) OpenNameKb();
            ImGui::SameLine();
            if (ImGui::Button("CREATE LOBBY", big)) CmdLobbyCreate();
            ImGui::SameLine();
            if (ImGui::Button("JOIN LOBBY", big)) OpenCodeKb();
            ImGui::Text("Code: %s", s_codeBuf);
            ImGui::SameLine();
            if (ImGui::Button("Join##do") && s_codeBuf[0])
                CmdLobbyJoin(s_codeBuf);
        } else {
            ImGui::Text("Lobby %s  (you: %.*s)", s.code, 10, s.myCid);
            ImGui::SameLine();
            if (ImGui::Button("Leave")) CmdLobbyLeave();

            /* pick mode (owner only) */
            bool owner = strcmp(s.ownerCid, s.myCid) == 0;
            ImGui::Text("Song pick: %s", s.pickMode);
            if (owner) {
                ImGui::SameLine();
                if (ImGui::RadioButton("rotate", strcmp(s.pickMode, "rotate") == 0))
                    CmdPickMode("rotate", nullptr);
                ImGui::SameLine();
                if (ImGui::RadioButton("owner", strcmp(s.pickMode, "owner") == 0))
                    CmdPickMode("owner", nullptr);
                ImGui::SameLine();
                if (ImGui::RadioButton("assigned", strcmp(s.pickMode, "assigned") == 0)) {
                    /* assign: next click on a member assigns them */
                    s_pickIdx = -2;
                    CmdPickMode("assigned", s.myCid);
                }
                if (strcmp(s.pickMode, "assigned") == 0)
                    ImGui::TextDisabled("(click a member to assign)");
            }

            ImGui::Separator();
            ImGui::Text("Members:");
            for (int i = 0; i < s.memberCount; i++) {
                const MemberInfo& m = s.members[i];
                bool isLead = strcmp(m.cid, s.leaderCid) == 0;
                bool isOwn  = strcmp(m.cid, s.ownerCid) == 0;
                bool isYou  = strcmp(m.cid, s.myCid) == 0;
                char line[160];
                snprintf(line, sizeof(line), "%s%s %s%s%s",
                         isOwn ? "[O] " : "",
                         isLead ? "* " : "  ",
                         m.name, isYou ? " (you)" : "",
                         isLead ? " - leader" : "");
                ImVec4 c = m.connected
                    ? (m.ready ? ImVec4(0.6f, 1, 0.6f, 1)
                               : ImVec4(1, 1, 1, 1))
                    : ImVec4(0.5f, 0.5f, 0.5f, 1);
                if (owner && strcmp(s.pickMode, "assigned") == 0) {
                    if (ImGui::Selectable(line, false)) {
                        CmdPickMode("assigned", m.cid);
                    }
                } else {
                    ImGui::TextColored(c, "%s", line);
                }
                ImGui::SameLine(0, 8 * k);
                ImGui::TextDisabled("[%s]", MemberStatus(m));
            }

            if (s.notReady[0])
                ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1),
                                   "Not ready: %s", s.notReady);
            if (s.statusLine[0])
                ImGui::Text("%s", s.statusLine);

            /* group pause state */
            if (s.groupPaused)
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1, 1), "GROUP PAUSED");
            if (s.voteKind[0])
                ImGui::Text("%s vote: %d/%d (%ds)",
                            strcmp(s.voteKind, "pause") == 0
                                ? "Pause" : "Resume",
                            s.voteAgreed, s.voteNeed, s.voteEndsIn);
        }

        EndWin();
        return;
    }

}

/* ================================================================
 * Banners (top-center, fading)
 * ============================================================== */
void DrawBanners() {
    Snapshot s;
    GetSnapshot(&s);
    if (s.bannerCount == 0) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float k = ds.y / 720.0f;
    for (int i = 0; i < s.bannerCount; i++) {
        float ttl = s.banners[i].ttl;
        float alpha = ttl > 2.5f ? (3.0f - ttl) * 2.0f
                    : (ttl < 0.8f ? ttl / 0.8f : 1.0f);
        const char* txt = s.banners[i].text;
        ImVec2 ts = ImGui::CalcTextSize(txt);
        ImGui::SetNextWindowPos(
            ImVec2((ds.x - ts.x) * 0.5f, 60 * k + i * 28 * k),
            ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f * alpha);
        char id[16];
        snprintf(id, sizeof(id), "##mpb%d", i);
        if (!BeginWin(id, nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_AlwaysAutoResize)) {
            EndWin();
            continue;
        }
        ImGui::TextColored(ImVec4(1, 1, 1, alpha), "%s", txt);
        EndWin();
    }
}

/* ================================================================
 * Leaderboard (in-song) + waiting + standings
 * ============================================================== */
void DrawLeaderboard() {
    Snapshot s;
    GetSnapshot(&s);
    if (s.boardCount == 0) return;
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x - 200 * k, 95 * k), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (!BeginWin("##mplb", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize)) {
        EndWin();
        return;
    }
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

void DrawWaiting() {
    Snapshot s;
    GetSnapshot(&s);
    if (s.playerCount == 0) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float k = ds.y / 720.0f;
    ImGui::SetNextWindowPos(
        ImVec2(ds.x * 0.5f - 150 * k, ds.y * 0.4f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (!BeginWin("##mpwait", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize)) {
        EndWin();
        return;
    }
    ImGui::Text("Waiting for players... %d/%d loaded",
                s.loadedCount, s.playerCount);
    ImGui::TextDisabled("don't unpause - the game resumes automatically");
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
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (!BeginWin("##mpstand", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize)) {
        EndWin();
        return;
    }
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

/* ================================================================
 * Emergency-exit progress circle (bottom-right while Minus held)
 * ============================================================== */
void DrawEmergency() {
    Snapshot s;
    GetSnapshot(&s);
    if (!s.minusHeld || s.minusHeldMs <= 0) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float k = ds.y / 720.0f;
    float frac = s.minusHeldMs / 3000.0f;
    if (frac > 1.0f) frac = 1.0f;
    ImVec2 ctr(ds.x - 80 * k, ds.y - 80 * k);
    float rad = 34 * k;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRectFullScreen();
    dl->AddCircleFilled(ctr, rad, IM_COL32(20, 20, 20, 180));
    dl->PathArcTo(ctr, rad - 6 * k, -1.5708f,
                  -1.5708f + 6.2832f * frac, 32);
    dl->PathStroke(IM_COL32(255, 80, 60, 255), 0, 6 * k);
    ImVec2 ts = ImGui::CalcTextSize("Hold to Emergency Exit Lobby");
    dl->AddText(ImVec2(ctr.x - ts.x * 0.5f, ctr.y + rad + 6 * k),
                IM_COL32(255, 255, 255, 220),
                "Hold to Emergency Exit Lobby");
    dl->PopClipRect();
}

} // namespace mp::lobby
