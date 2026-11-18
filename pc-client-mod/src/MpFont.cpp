// PC port of FontLoader.cpp — see MpFont.hpp.
#include "MpFont.hpp"
#include "platform/MpLog.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdio>
#include <string>

namespace FontLoader {

static ImFont* s_big = nullptr;
static ImFont* s_countdown = nullptr;

ImFont* Big()       { return s_big; }
ImFont* Countdown() { return s_countdown; }

static bool FileExists(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (f) { fclose(f); return true; }
    return false;
}

/* glyph range covering " !0-9GO" like the Switch countdown font */
static const ImWchar s_countdownGlyphs[] = {
    0x20, 0x21,   /* ' ' '!' */
    0x30, 0x39,   /* 0-9 */
    0x47, 0x47,   /* G */
    0x4F, 0x4F,   /* O */
    0,
};

void LoadFonts(ImGuiIO& io, const char* modDirA, float scale720) {
    if (scale720 <= 0.0f) scale720 = 1.0f;
    float basePx = 11.0f * scale720;
    if (basePx < 11.0f) basePx = 11.0f;
    float bigPx  = 36.0f * scale720;
    float ctdPx  = 150.0f * scale720;

    std::string mod = modDirA ? modDirA : "";
    std::string mpFont = mod + "fonts\\mp_font.ttf";
    bool hasMpFont = FileExists(mpFont);

    std::string segoe    = "C:\\Windows\\Fonts\\segoeui.ttf";
    std::string segoeB   = "C:\\Windows\\Fonts\\segoeuib.ttf";
    std::string jpFont   = "C:\\Windows\\Fonts\\YuGothM.ttc";
    if (!FileExists(jpFont)) jpFont = "C:\\Windows\\Fonts\\msgothic.ttc";
    std::string jpAlt    = "C:\\Windows\\Fonts\\YuGothR.ttc";

    io.Fonts->Clear();
    s_big = nullptr;
    s_countdown = nullptr;

    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;

    /* base font: mp_font.ttf or segoeui, then merge Japanese ranges */
    const char* basePath = hasMpFont ? mpFont.c_str() : segoe.c_str();
    io.FontDefault = io.Fonts->AddFontFromFileTTF(basePath, basePx, &cfg);
    if (!io.FontDefault && hasMpFont)
        io.FontDefault = io.Fonts->AddFontFromFileTTF(segoe.c_str(), basePx, &cfg);
    if (!io.FontDefault)
        io.FontDefault = io.Fonts->AddFontDefault();

    const ImWchar* jpRanges = io.Fonts->GetGlyphRangesJapanese();
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.OversampleH = 3;
    merge.OversampleV = 2;
    if (!io.Fonts->AddFontFromFileTTF(jpFont.c_str(), basePx, &merge, jpRanges) && FileExists(jpAlt))
        io.Fonts->AddFontFromFileTTF(jpAlt.c_str(), basePx, &merge, jpRanges);

    const char* dispPath = hasMpFont ? mpFont.c_str() : segoeB.c_str();
    s_big = io.Fonts->AddFontFromFileTTF(dispPath, bigPx, &cfg,
            io.Fonts->GetGlyphRangesDefault());
    if (!s_big)
        s_big = io.Fonts->AddFontFromFileTTF(segoeB.c_str(), bigPx, &cfg,
                io.Fonts->GetGlyphRangesDefault());

    s_countdown = io.Fonts->AddFontFromFileTTF(dispPath, ctdPx, &cfg,
                      s_countdownGlyphs);
    if (!s_countdown)
        s_countdown = io.Fonts->AddFontFromFileTTF(segoeB.c_str(), ctdPx, &cfg,
                          s_countdownGlyphs);

    WriteLog("[MP] fonts: base=%s big=%s ctd=%s scale=%.2f\n",
             basePath, s_big ? "ok" : "fallback", s_countdown ? "ok" : "fallback",
             scale720);
}

} // namespace FontLoader
