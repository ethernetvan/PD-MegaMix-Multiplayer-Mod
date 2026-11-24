#pragma once
// PC port of the Switch FontLoader: base UI font, "Big" and "Countdown"
// display fonts. Sizes are specified at 720p and scaled by
// backbuffer_height/720, same as the Switch overlay.
struct ImFont;
struct ImGuiIO;

namespace FontLoader {

/* (Re)build the atlas for the given 720p-reference scale.
 * Prefer <moddir>\fonts\mp_font.ttf when present; else Segoe UI / Segoe UI
 * Bold + a Japanese fallback (Yu Gothic, then MS Gothic) merged into the base
 * font so Japanese names render. */
void LoadFonts(ImGuiIO& io, const char* modDirA, float scale720);

ImFont* Big();
ImFont* Countdown();

} // namespace FontLoader
