// PC status chips: passive "MP: ..." chip (bottom-left when debug_status_chip
// is on). The song-select badge is mp::lobby::DrawSelBadge in MpUi.cpp.
#include "MpLobby.hpp"
#include "MpNet.hpp"
#include "../Config.hpp"
#include <imgui.h>
#include <cstdio>

static void ChipColored(ImVec4 color, const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::TextColored(color, "%s", buf);
}

static ImVec4 StateColor(mp::MpState st) {
    switch (st) {
    case mp::MpState::Connected: return ImVec4(0.30f, 0.85f, 0.45f, 1.0f);
    case mp::MpState::Error:     return ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
    case mp::MpState::Disabled:  return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    default:                     return ImVec4(1.0f, 0.85f, 0.25f, 1.0f);
    }
}

/* bottom-left debug chip (mpShowStatus) */
static void DrawStatusChip() {
    if (!Config::mpShowStatus) return;
    mp::MpStatus st;
    mp::GetStatus(&st);
    ImVec4 color = StateColor(st.state);
    float s = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImGui::SetNextWindowPos(
        ImVec2(12.0f * s, ImGui::GetIO().DisplaySize.y - 12.0f * s),
        ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 4.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * s);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(color.x, color.y, color.z, 0.45f));
    if (ImGui::Begin("###MpStatusChip", nullptr, flags)) {
        if (st.state == mp::MpState::Connected)
            ChipColored(color, "MP: %s %s %d ms", mp::StateName(st.state),
                        st.server, st.rtt_ms);
        else if (st.last_error[0])
            ChipColored(color, "MP: %s %s (%s)", mp::StateName(st.state),
                        st.server, st.last_error);
        else
            ChipColored(color, "MP: %s %s", mp::StateName(st.state), st.server);
    }
    ImGui::End();
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(2);
}

/* called from mod.cpp each frame */
void MpStatusChipDraw() {
    if (!Config::mpEnabled) return;
    DrawStatusChip();
}
