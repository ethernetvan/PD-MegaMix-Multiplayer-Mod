#pragma once

#include "imgui/imgui.h"

namespace FontLoader {
    void LoadFonts(ImGuiIO& io);
    /* Large display font (36px at 720p), nullptr when only the default
     * font exists — callers should fall back to ImGui::GetFont(). */
    ImFont* Big();
}