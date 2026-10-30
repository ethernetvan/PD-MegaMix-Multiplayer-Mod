#include "MpOverlay.hpp"
#include "MpNet.hpp"
#include "../Config.hpp"
#include "imgui/imgui.h"

namespace MpOverlay {

bool IsVisible() {
    return Config::mpEnabled && Config::mpShowStatus;
}

void DrawWindow() {
    if (!IsVisible()) return;

    mp::MpStatus st;
    mp::GetStatus(&st);

    ImVec4 color;
    switch (st.state) {
        case mp::MpState::Connected:      color = ImVec4(0.30f, 0.85f, 0.45f, 1.0f); break;
        case mp::MpState::Error:          color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);  break;
        case mp::MpState::Disabled:       color = ImVec4(0.60f, 0.60f, 0.60f, 1.0f); break;
        default:                          color = ImVec4(1.0f, 0.85f, 0.25f, 1.0f);  break;
    }

    float s = ImGui::GetIO().DisplaySize.y / 720.0f;

    ImGui::SetNextWindowPos(ImVec2(12.0f * s, ImGui::GetIO().DisplaySize.y - 12.0f * s),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s, 4.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * s);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(color.x, color.y, color.z, 0.45f));

    if (ImGui::Begin("###MpStatusChip", nullptr, flags)) {
        ImGui::GetWindowDrawList()->PushClipRectFullScreen();
        if (st.state == mp::MpState::Connected)
            ImGui::TextColored(color, "MP: %s %s %d ms", mp::StateName(st.state),
                               st.server, st.rtt_ms);
        else if (st.last_error[0])
            ImGui::TextColored(color, "MP: %s %s (%s)", mp::StateName(st.state),
                               st.server, st.last_error);
        else
            ImGui::TextColored(color, "MP: %s %s", mp::StateName(st.state),
                               st.server);
        ImGui::GetWindowDrawList()->PopClipRect();
    }
    ImGui::End();

    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(2);
}

} // namespace MpOverlay
