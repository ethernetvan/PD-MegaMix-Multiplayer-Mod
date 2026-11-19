// PC panel for the MP lobby — desktop-style ImGui window (the Switch
// system-menu panel is not ported). Two tabs: Lobby and Settings.
// Toggle: F8 or `dbg_cmd overlay open|close`; Esc closes.
#include "MpLobby.hpp"
#include "MpAvatar.hpp"
#include "MpNet.hpp"
#include "../Config.hpp"
#include "../game/MpAutoPlay.hpp"
#include "../platform/MpLog.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace mp::lobby {

bool PadModeActive() { return false; }   /* PC is pointer/keyboard driven */
void RequestPadMode() {}

/* ---------------- shared pieces ---------------- */
static const ImU32 kTeal   = IM_COL32(57, 197, 187, 255);   /* #39C5BB */
static const ImU32 kMuted  = IM_COL32(150, 155, 162, 255);
static const ImU32 kGreen  = IM_COL32(90, 205, 130, 255);
static const ImU32 kRed    = IM_COL32(235, 85, 95, 255);
static const ImU32 kYellow = IM_COL32(255, 200, 80, 255);

/* pointy-top hexagon avatar (same helper shape as the in-game draws) */
static void HexPts(ImVec2 c, float r, ImVec2 out[6], float rotDeg = -90.0f) {
    for (int i = 0; i < 6; i++) {
        float a = (rotDeg + 60.0f * i) * 3.14159265f / 180.0f;
        out[i] = ImVec2(c.x + r * cosf(a), c.y + r * sinf(a));
    }
}
/* triangle-fan fill of a pointy-top hexagon (portrait clipped to the hex
 * shape, same as the race/results draws in MpUi.cpp) */
static void HexTex(ImDrawList* dl, ImVec2 c, float r, ImTextureID tex,
                   ImVec2 uv0, ImVec2 uv1) {
    ImVec2 p[7];
    p[0] = c;
    HexPts(c, r, p + 1);
    dl->PushTextureID(tex);
    dl->PrimReserve(18, 7);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < 7; i++) {
        ImVec2 uv(uv0.x + (p[i].x - c.x + r) / (2.0f * r) * (uv1.x - uv0.x),
                  uv0.y + (p[i].y - c.y + r) / (2.0f * r) * (uv1.y - uv0.y));
        dl->PrimWriteVtx(p[i], uv, IM_COL32_WHITE);
    }
    for (int i = 0; i < 6; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % 6));
    }
    dl->PopTextureID();
}

static void DrawHexAvatar(ImDrawList* dl, ImVec2 c, float r,
                          const mp::avatar::Ref& av, bool connected) {
    ImVec2 pts[6];
    HexPts(c, r, pts);
    void* tex = nullptr; float uv0[2], uv1[2];
    if (mp::avatar::Resolve(av, &tex, uv0, uv1) == 1 && tex) {
        HexTex(dl, c, r, (ImTextureID)tex, ImVec2(uv0[0], uv0[1]),
               ImVec2(uv1[0], uv1[1]));
    } else {
        dl->AddConvexPolyFilled(pts, 6, IM_COL32(60, 66, 74, 255));
        /* silhouette: head + shoulders */
        ImVec2 sc = ImVec2(c.x, c.y - r * 0.18f);
        dl->AddCircleFilled(sc, r * 0.26f, IM_COL32(120, 128, 140, 255));
        ImVec2 q[4] = {
            ImVec2(c.x - r * 0.34f, c.y + r * 0.42f),
            ImVec2(c.x - r * 0.28f, c.y + r * 0.14f),
            ImVec2(c.x + r * 0.28f, c.y + r * 0.14f),
            ImVec2(c.x + r * 0.34f, c.y + r * 0.42f),
        };
        dl->AddConvexPolyFilled(q, 4, IM_COL32(120, 128, 140, 255));
    }
    dl->AddPolyline(pts, 6, connected ? kTeal : IM_COL32(90, 95, 105, 255),
                    ImDrawFlags_Closed, 1.5f);
}

static void StatusDot(ImVec2 c, bool connected) {
    ImGui::GetWindowDrawList()->AddCircleFilled(
        c, 3.5f, connected ? kGreen : kRed);
}

/* ---------------- Lobby tab ---------------- */
static char s_nameBuf[40] = "";
static bool s_nameInit = false;
static char s_codeBuf[8] = "";
static char s_urlBuf[160] = "";
static bool s_urlInit = false;
static char s_kickCid[20] = "";
static bool s_confirmLeave = false;

static void LobbyTab(const Snapshot& sn) {
    float sc = ImGui::GetIO().DisplaySize.y / 1080.0f;
    if (sc < 1.0f) sc = 1.0f;

    /* --- player name --- */
    if (!s_nameInit) {
        strncpy(s_nameBuf, Config::mpPlayerName.c_str(), sizeof(s_nameBuf) - 1);
        s_nameInit = true;
    }
    ImGui::Text("Player name");
    ImGui::SameLine(120 * sc);
    ImGui::SetNextItemWidth(220 * sc);
    bool submit = ImGui::InputText("##pname", s_nameBuf, sizeof(s_nameBuf),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Save##name") || submit) {
        if (s_nameBuf[0] && strncmp(s_nameBuf, Config::mpPlayerName.c_str(),
                                    sizeof(s_nameBuf)) != 0)
            CmdSetName(s_nameBuf);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!sn.code[0]) {
        /* --- not in a room --- */
        if (ImGui::Button("Create room", ImVec2(160 * sc, 0)))
            CmdLobbyCreate();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * sc);
        ImGui::InputTextWithHint("##code", "CODE", s_codeBuf, sizeof(s_codeBuf),
                                 ImGuiInputTextFlags_CharsUppercase |
                                 ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Join") && s_codeBuf[0])
            CmdLobbyJoin(s_codeBuf);
        if (ImGui::IsItemDeactivatedAfterEdit() && s_codeBuf[0]) { /* keep */ }
        ImGui::Spacing();
        ImGui::TextDisabled("Create a room or join one with a code.");
    } else {
        /* --- in a room --- */
        ImGui::Text("Room");
        ImGui::SameLine();
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]);
        ImGui::TextColored(ImVec4(0.224f, 0.773f, 0.733f, 1.0f), "%s", sn.code);
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy"))
            ImGui::SetClipboardText(sn.code);
        ImGui::SameLine();
        ImGui::TextDisabled("round %d", sn.round);

        ImGui::Spacing();
        /* member list */
        ImGui::BeginChild("##members", ImVec2(0, 200 * sc), true,
                          ImGuiWindowFlags_NoScrollbar);
        for (int i = 0; i < sn.memberCount; i++) {
            const MemberInfo& m = sn.members[i];
            ImGui::PushID(i);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 rowMin = ImGui::GetCursorScreenPos();
            float avR = 10.0f * sc;
            DrawHexAvatar(dl, ImVec2(rowMin.x + avR + 4, rowMin.y + avR + 3),
                          avR, m.av, m.connected);
            ImGui::Dummy(ImVec2(avR * 2 + 10, avR * 2 + 6));
            ImGui::SameLine();
            ImGui::TextUnformatted(m.name[0] ? m.name : "(unnamed)");
            if (strcmp(m.cid, sn.myCid) == 0)
                { ImGui::SameLine(); ImGui::TextDisabled("(you)"); }
            if (strcmp(m.cid, sn.ownerCid) == 0)
                { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.224f,0.773f,0.733f,1), "Owner"); }
            if (sn.assignedCid[0] && strcmp(sn.assignedCid, "null") != 0 &&
                strcmp(m.cid, sn.assignedCid) == 0)
                { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f,0.78f,0.31f,1), "Picker"); }
            if (m.is_leader)
                { ImGui::SameLine(); ImGui::TextDisabled("Leader"); }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m.ready ? "ready" :
                                  (m.reason[0] ? m.reason : "not ready"));
            /* status dot + kick right-aligned on the same line */
            ImGui::SameLine();
            float rightX = ImGui::GetWindowContentRegionMax().x;
            bool canKick = strcmp(sn.myCid, sn.ownerCid) == 0 &&
                           strcmp(m.cid, sn.myCid) != 0;
            ImGui::SetCursorPosX(rightX - (canKick ? 58 * sc : 12 * sc));
            ImVec2 dot = ImGui::GetCursorScreenPos();
            StatusDot(ImVec2(dot.x + 4, dot.y + 7), m.connected);
            if (canKick) {
                ImGui::SetCursorPosX(rightX - 46 * sc);
                if (ImGui::SmallButton("Kick")) {
                    strncpy(s_kickCid, m.cid, sizeof(s_kickCid) - 1);
                    ImGui::OpenPopup("Kick?");
                }
            } else {
                /* end the row with an item: after the SameLine above, a
                 * row with no Kick button left the next member on this
                 * line (rows ran off to the right) */
                ImGui::Dummy(ImVec2(12 * sc, 14 * sc));
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        /* kick confirmation */
        if (ImGui::BeginPopupModal("Kick?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const char* who = s_kickCid;
            for (int i = 0; i < sn.memberCount; i++)
                if (strcmp(sn.members[i].cid, s_kickCid) == 0)
                    who = sn.members[i].name;
            ImGui::Text("Kick %s from the room?", who);
            if (ImGui::Button("Kick", ImVec2(100 * sc, 0))) {
                CmdKick(s_kickCid);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100 * sc, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        /* owner controls */
        if (strcmp(sn.myCid, sn.ownerCid) == 0) {
            ImGui::Spacing();
            ImGui::Text("Pick mode");
            ImGui::SameLine(120 * sc);
            ImGui::SetNextItemWidth(180 * sc);
            const char* modes[] = { "rotate", "owner", "assigned" };
            int mi = strncmp(sn.pickMode, "assigned", 8) == 0 ? 2 :
                     strncmp(sn.pickMode, "owner", 5) == 0 ? 1 : 0;
            if (ImGui::Combo("##pickmode", &mi, modes, 3)) {
                if (mi == 2) {
                    /* the server needs a member to assign: keep a valid
                     * assignment, else the current picker, else me */
                    const char* who =
                        (sn.assignedCid[0] && strcmp(sn.assignedCid, "null") != 0)
                            ? sn.assignedCid
                            : (sn.leaderCid[0] && strcmp(sn.leaderCid, "null") != 0)
                                ? sn.leaderCid : sn.myCid;
                    CmdPickMode("assigned", who);
                } else {
                    CmdPickMode(modes[mi], nullptr);
                }
            }
            ImGui::SameLine();
            ImGui::Text("Picker");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160 * sc);
            const char* assignedName = "(none)";
            bool assignedValid = sn.assignedCid[0] &&
                                 strcmp(sn.assignedCid, "null") != 0;
            if (assignedValid)
                for (int i = 0; i < sn.memberCount; i++)
                    if (strcmp(sn.members[i].cid, sn.assignedCid) == 0)
                        assignedName = sn.members[i].name;
            ImGui::BeginDisabled(mi != 2);
            if (ImGui::BeginCombo("##assign", assignedName)) {
                for (int i = 0; i < sn.memberCount; i++) {
                    if (ImGui::Selectable(sn.members[i].name))
                        CmdPickMode("assigned", sn.members[i].cid);
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        if (sn.selPv > 0)
            ImGui::Text("Song: %s (pv %d)", sn.selName, sn.selPv);
        if (sn.statusLine[0])
            ImGui::TextDisabled("%s", sn.statusLine);
        if (sn.notReady[0])
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.31f, 1), "%s", sn.notReady);
        if (sn.unavailFor[0])
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.31f, 1),
                               "unavailable for: %s", sn.unavailFor);

        ImGui::Spacing();
        if (ImGui::Button("Leave room", ImVec2(140 * sc, 0)) && !s_confirmLeave)
            s_confirmLeave = true;
        if (s_confirmLeave) {
            ImGui::SameLine();
            ImGui::Text("Leave the room?");
            ImGui::SameLine();
            if (ImGui::Button("Yes")) { CmdLobbyLeave(); s_confirmLeave = false; }
            ImGui::SameLine();
            if (ImGui::Button("No")) s_confirmLeave = false;
        }
    }
}

/* ---------------- Settings tab ---------------- */
static void SettingsTab(const Snapshot& sn) {
    (void)sn;
    float sc = ImGui::GetIO().DisplaySize.y / 1080.0f;
    if (sc < 1.0f) sc = 1.0f;

    /* server mode radio */
    ImGui::Text("Server");
    int mode = Config::mpServerMode == "cloud" ? 2 :
               Config::mpServerMode == "off" ? 0 : 1;
    if (ImGui::RadioButton("Off", &mode, 0))    mp::SetServerMode(kServerOff);
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", &mode, 1))  mp::SetServerMode(kServerLocal);
    ImGui::SameLine();
    if (ImGui::RadioButton("Online", &mode, 2)) mp::SetServerMode(kServerCloud);

    if (mode == 2) {
        if (!s_urlInit) {
            strncpy(s_urlBuf, Config::mpCloudUrl.c_str(), sizeof(s_urlBuf) - 1);
            s_urlInit = true;
        }
        ImGui::Text("Online address");
        ImGui::SameLine(120 * sc);
        ImGui::SetNextItemWidth(300 * sc);
        bool ap = ImGui::InputText("##url", s_urlBuf, sizeof(s_urlBuf),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Apply") || ap)
            mp::SetCloudUrl(s_urlBuf);
    } else {
        ImGui::TextDisabled("Local: LAN discovery + server_ip %s:%d",
                            Config::mpServerIp.c_str(), Config::mpServerPort);
    }

    ImGui::Spacing();
    /* notifications */
    ImGui::Text("Notifications");
    ImGui::SameLine(160 * sc);
    const char* styles[] = { "Portraits", "Text", "Off" };
    int st = Config::mpNotifyStyle == "text" ? 1 :
             Config::mpNotifyStyle == "off" ? 2 : 0;
    if (ImGui::Combo("##notify", &st, styles, 3)) {
        Config::mpNotifyStyle = st == 1 ? "text" : st == 2 ? "off" : "avatars";
        Config::Save();
    }
    bool ra = Config::mpRemoteAvatars;
    if (ImGui::Checkbox("Custom portraits", &ra)) {
        Config::mpRemoteAvatars = ra;
        Config::Save();
    }
    bool chip = Config::mpShowStatus;
    if (ImGui::Checkbox("Status chip", &chip)) {
        Config::mpShowStatus = chip;
        Config::Save();
    }

    ImGui::Spacing();
    /* AutoPlay */
    ImGui::Text("AutoPlay");
    ImGui::SameLine(160 * sc);
    bool ap = AutoPlay::g_enabled;
    if (ImGui::Checkbox("##autoplay", &ap)) {
        AutoPlay::g_enabled = ap;
        ReportAutoPlay(ap);
    }
    if (AutoPlay::g_enabled) {
        static const char* const kApModes[] = {
            "Custom", "Auto", "Pro", "97", "Streak", "Human", "Clutch", "Tie"
        };
        ImGui::TextDisabled("Mode");
        ImGui::SameLine(160 * sc);
        int am = AutoPlay::g_mode;
        if (am < 0 || am >= AutoPlay::Mode_Count) am = AutoPlay::Mode_Auto;
        ImGui::SetNextItemWidth(160 * sc);
        if (ImGui::Combo("##apmode", &am, kApModes, AutoPlay::Mode_Count)) {
            AutoPlay::g_mode = am;
            ReportAutoPlay(true);
        }
        if (AutoPlay::g_mode == AutoPlay::Mode_Custom) {
            ImGui::TextDisabled("Hit %");
            ImGui::SameLine(160 * sc);
            int pct = AutoPlay::g_hitPct;
            ImGui::SetNextItemWidth(200 * sc);
            if (ImGui::SliderInt("##appct", &pct, 0, 100)) {
                AutoPlay::g_hitPct = pct;
                ReportAutoPlay(true);
            }
        }
    }
}

/* ---------------- main panel ---------------- */
void DrawPanel() {
    static bool s_prevOpen = false;
    bool open = OverlayOpen();
    if (open != s_prevOpen) {
        s_prevOpen = open;
        if (open) {
            s_nameInit = false;
            s_urlInit = false;
            s_confirmLeave = false;
        }
    }
    if (!open) return;

    ImGuiIO& io = ImGui::GetIO();
    float sc = io.DisplaySize.y / 1080.0f;
    if (sc < 1.0f) sc = 1.0f;
    ImVec2 sz(760 * sc, 540 * sc);
    ImGui::SetNextWindowSize(sz, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,
                                   io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.94f);

    Snapshot sn;
    GetSnapshot(&sn);

    /* flat dark neutral + DIVA teal accent */
    const ImVec4 kWin(0.106f, 0.118f, 0.137f, 1.0f);   /* #1B1E23 */
    const ImVec4 kFrame(0.149f, 0.165f, 0.192f, 1.0f); /* #262A31 */
    const ImVec4 kTeal(0.224f, 0.773f, 0.733f, 1.0f);  /* #39C5BB */
    const ImVec4 kTealHov(0.30f, 0.82f, 0.77f, 1.0f);
    const ImVec4 kTealAct(0.18f, 0.62f, 0.58f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kWin);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kFrame);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, kWin);
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, kFrame);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kFrame);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(kFrame.x * 1.25f, kFrame.y * 1.25f, kFrame.z * 1.25f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kFrame);
    ImGui::PushStyleColor(ImGuiCol_Button, kTealAct);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kTealHov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kTeal);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, kTealHov);
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, kTeal);
    ImGui::PushStyleColor(ImGuiCol_SeparatorHovered, kTealHov);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, kTealAct);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kTeal);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(kFrame.x * 1.6f, kFrame.y * 1.6f, kFrame.z * 1.6f, 1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kTeal);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(kTeal.x, kTeal.y, kTeal.z, 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Multiplayer###mppanel", nullptr, flags)) {
        /* light backdrop */
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                          IM_COL32(0, 0, 0, 90));

        /* footer (before content so it's pinned) */
        ImVec2 winPos = ImGui::GetWindowPos();
        ImVec2 winSz  = ImGui::GetWindowSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(winPos.x + 8, winPos.y + winSz.y - 30 * sc),
                    ImVec2(winPos.x + winSz.x - 8, winPos.y + winSz.y - 30 * sc),
                    IM_COL32(255, 255, 255, 30));
        MpStatus mst;
        mp::GetStatus(&mst);
        char foot[160];
        if (mst.state == MpState::Connected)
            snprintf(foot, sizeof(foot), "%s  %s  %d ms      F8 close",
                     mp::StateName(mst.state), mst.server, mst.rtt_ms);
        else if (mst.last_error[0])
            snprintf(foot, sizeof(foot), "%s  %s  (%s)      F8 close",
                     mp::StateName(mst.state), mst.server, mst.last_error);
        else
            snprintf(foot, sizeof(foot), "%s  %s      F8 close",
                     mp::StateName(mst.state), mst.server);
        dl->AddText(ImVec2(winPos.x + 12 * sc, winPos.y + winSz.y - 24 * sc),
                    kMuted, foot);

        ImGui::BeginTabBar("##tabs");
        if (ImGui::BeginTabItem("Lobby")) {
            ImGui::Spacing();
            LobbyTab(sn);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
            ImGui::Spacing();
            SettingsTab(sn);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(21);
}

} // namespace mp::lobby
