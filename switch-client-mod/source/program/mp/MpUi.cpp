#include "MpLobby.hpp"
#include "MpAvatar.hpp"
#include "MpInput.hpp"
#include "MpNet.hpp"
#include "../AutoPlay.hpp"
#include "../Config.hpp"
#include "../FontLoader.hpp"
#include "../keyboard.hpp"
#include "../overlays/VirtualKeyboard.hpp"

namespace ImGui { extern bool g_imguiHasFocus; }
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
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

/* ================================================================
 * Multiplayer overlay — Switch-system-menu styled, touch/cursor driven.
 *
 *   HOME        three cards: Edit Name / Create Room / Join Room
 *   EDIT_NAME   title + text field + CANCEL/SAVE, keyboard below
 *   JOIN_ROOM   title + code field + CANCEL/JOIN, keyboard below
 *   ROOM        podium (top 3) + ordered member list w/ kick, Room
 *               Config (owner) and Exit Room
 *   ROOM_CONFIG song-picker mode + assignee (owner only)
 *   OPTIONS     server (off / local / online), notification style,
 *               custom portraits (from HOME)
 *   EDIT_SERVER online server address + CANCEL/SAVE, keyboard below
 *
 * In a room the overlay always opens straight on ROOM.
 * ============================================================== */

enum Screen { SC_HOME, SC_EDIT_NAME, SC_JOIN_ROOM, SC_ROOM, SC_ROOM_CONFIG,
              SC_OPTIONS, SC_EDIT_SERVER, SC_WEB_KEY };

static Screen s_screen   = SC_HOME;
static char   s_codeBuf[8]  = "";
static char   s_nameBuf[40] = "";
static char   s_urlBuf[100] = "";
static bool   s_kbArmed  = false;   /* keyboard was opened for this screen */
static bool   s_prevOpen = false;
/* JOIN_ROOM stays up after JOIN: the room screen replaces it once the
 * server lets us in, a refusal shows next to the title */
static char   s_codeSent[8] = "";   /* s_codeBuf as it was submitted */
static bool   s_joinShown = false;  /* show that join's outcome */
static bool   s_joinKbBack = false; /* submitting closed the keyboard: reopen */

/* ============================ pad navigation ============================
 * Focusables are registered while each screen draws (same order every
 * frame, so the push index is stable). Pad mode is entered on any nav
 * button/stick input and left on any touch/mouse movement; in pad mode
 * Ui::FHover draws the cyan ring on the focused item and FActivated
 * fires on A. Touch behaviour is unchanged in pointer mode. */
struct FocusItem { ImVec2 a, b; bool enabled; bool danger; };
static FocusItem s_items[24];
static int       s_itemCount = 0;
static int       s_focusIdx = 0;
static bool      s_padMode = false;
static bool      s_padA = false;        /* A edge consumed this frame */
static Screen    s_lastScreen = SC_HOME;
static char      s_armCid[20] = "";     /* kick-armed member (2-step pad kick) */
static uint64_t  s_armUntil = 0;
static uint64_t  s_zrIgnoreUntil = 0;   /* ignore ZR edges right after open */
static bool      s_exitArmed = false;   /* Exit Room armed (2-step pad) */
static uint64_t  s_exitArmUntil = 0;
static bool      s_padModeReq = false;  /* pad open requested (e.g. via ZR) */
static uint64_t  s_holdStart[2] = {0, 0};
static uint64_t  s_lastStep[2] = {0, 0};
static int       s_lastDir[2] = {0, 0};

static uint64_t NavNowMs() {
    return (uint64_t)nn::os::GetSystemTick().GetInt64Value() * 1000
           / nn::os::GetSystemTickFrequency();
}

/* ---- palette (Switch dark theme + Diva accents) ---- */
static const ImU32 kBg        = IM_COL32(45, 45, 45, 246);
static const ImU32 kCard      = IM_COL32(62, 62, 62, 255);
static const ImU32 kCardHov   = IM_COL32(76, 76, 76, 255);
static const ImU32 kCardDown  = IM_COL32(90, 90, 90, 255);
static const ImU32 kField     = IM_COL32(30, 30, 30, 255);
static const ImU32 kLine      = IM_COL32(255, 255, 255, 48);
static const ImU32 kText      = IM_COL32(255, 255, 255, 255);
static const ImU32 kMuted     = IM_COL32(175, 175, 175, 255);
static const ImU32 kDim       = IM_COL32(120, 120, 120, 255);
static const ImU32 kAccent    = IM_COL32(0, 195, 227, 255);   /* Switch focus cyan */
static const ImU32 kAccentBg  = IM_COL32(0, 195, 227, 40);
static const ImU32 kGreen     = IM_COL32(90, 205, 130, 255);
static const ImU32 kGreenBg   = IM_COL32(90, 205, 130, 40);
static const ImU32 kYellow    = IM_COL32(255, 200, 80, 255);
static const ImU32 kRed       = IM_COL32(235, 85, 95, 255);
static const ImU32 kRedBg     = IM_COL32(235, 85, 95, 45);
static const ImU32 kInk       = IM_COL32(28, 32, 36, 255);

struct Ui {
    ImDrawList* dl;
    ImFont*     font;
    float       k;        /* 720p scale */
    float       fs;       /* base font size */
    ImVec2      mouse;
    bool        clicked;
    bool        down;
    float       t;        /* seconds, for pulses */

    bool Hover(ImVec2 a, ImVec2 b) const {
        return mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y;
    }
    /* register a focusable; returns its index for this frame */
    int Focus(ImVec2 a, ImVec2 b, bool enabled = true, bool danger = false) const {
        if (s_itemCount < (int)(sizeof(s_items) / sizeof(s_items[0])))
            s_items[s_itemCount] = {a, b, enabled, danger};
        return s_itemCount++;
    }
    /* "hover" visual: pointer -> mouse hit-test; pad -> focused item */
    bool FHover(int idx) const {
        if (idx < 0 || idx >= s_itemCount) return false;
        if (!s_padMode) return Hover(s_items[idx].a, s_items[idx].b);
        return idx == s_focusIdx && s_items[idx].enabled;
    }
    /* activation: pointer -> mouse click; pad -> A on the focused item */
    bool FActivated(int idx) const {
        if (idx < 0 || idx >= s_itemCount) return false;
        if (!s_padMode) return Hover(s_items[idx].a, s_items[idx].b) && clicked;
        return idx == s_focusIdx && s_items[idx].enabled && s_padA;
    }
    bool FPadActivated(int idx) const {
        return s_padMode && idx == s_focusIdx && idx < s_itemCount &&
               s_items[idx].enabled && s_padA;
    }
    void Text(ImVec2 p, const char* s, float scale = 1.0f, ImU32 col = kText) const {
        dl->AddText(font, fs * scale, p, col, s);
    }
    ImVec2 Size(const char* s, float scale = 1.0f) const {
        return font->CalcTextSizeA(fs * scale, FLT_MAX, 0.0f, s);
    }
    void TextCentered(ImVec2 a, ImVec2 b, const char* s, float scale, ImU32 col) const {
        ImVec2 sz = Size(s, scale);
        dl->AddText(font, fs * scale,
                    ImVec2(a.x + (b.x - a.x - sz.x) * 0.5f, a.y + (b.y - a.y - sz.y) * 0.5f),
                    col, s);
    }
    /* copy s into out, trimming with ".." so it fits maxW (the NVN backend
     * ignores sub-rect clip rects, so we must never rely on clipping) */
    const char* Fit(const char* s, float maxW, float scale, char* out, int n) const {
        if (Size(s, scale).x <= maxW) { strncpy(out, s, n - 1); out[n - 1] = 0; return out; }
        int len = (int)strlen(s);
        while (len > 0) {
            /* don't split a UTF-8 sequence */
            do { len--; } while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80);
            snprintf(out, n, "%.*s..", len, s);
            if (Size(out, scale).x <= maxW) break;
        }
        return out;
    }
    /* text, left aligned, vertically centered in [a,b], trimmed to fit */
    void TextLeft(ImVec2 a, ImVec2 b, const char* s, float scale, ImU32 col) const {
        char buf[160];
        Fit(s, b.x - a.x, scale, buf, sizeof(buf));
        ImVec2 sz = Size(buf, scale);
        dl->AddText(font, fs * scale, ImVec2(a.x, a.y + (b.y - a.y - sz.y) * 0.5f), col, buf);
    }

    enum BtnStyle { B_NORMAL, B_PRIMARY, B_DANGER, B_GHOST };
    /* Switch style flat button: rounded rect, cyan focus ring on hover */
    bool Button(ImVec2 a, ImVec2 b, const char* label, BtnStyle st = B_NORMAL,
                float scale = 1.1f, bool enabled = true,
                bool focusable = true) const {
        int fi = Focus(a, b, enabled && focusable);
        bool hov = enabled && FHover(fi);
        bool prs = !s_padMode && hov && down;
        ImU32 bg = kCard, fg = kText, border = kLine;
        float r = 10.0f * k;
        switch (st) {
            case B_PRIMARY: bg = prs ? kAccent : kAccentBg; border = kAccent;
                            fg = prs ? kInk : kText; break;
            case B_DANGER:  bg = prs ? kRed : kRedBg; border = kRed;
                            fg = prs ? kInk : kText; break;
            case B_GHOST:   bg = IM_COL32(0, 0, 0, 0); border = IM_COL32(0, 0, 0, 0); break;
            default:        bg = prs ? kCardDown : (hov ? kCardHov : kCard); break;
        }
        if (!enabled) { fg = kDim; }
        dl->AddRectFilled(a, b, bg, r);
        if (hov) {
            Ring(a, b, r);
        } else if (st != B_GHOST) {
            dl->AddRect(a, b, border, r, 0, 1.2f);
        }
        TextCentered(a, b, label, scale, fg);
        return enabled && FActivated(fi);
    }
    /* focus ring: bold pulsing ring + soft outer glow in pad mode,
     * thin pulse for pointer hover */
    void Ring(ImVec2 a, ImVec2 b, float r, float gap = 2.0f) const {
        if (s_padMode) {
            float g = (gap + 3.0f) * k;
            ImU32 glow = (kAccent & 0x00FFFFFF) | (70u << 24);
            dl->AddRect(ImVec2(a.x - g, a.y - g), ImVec2(b.x + g, b.y + g),
                        glow, r + g, 0, 3.0f * k);
        }
        float pulse = s_padMode ? (0.85f + 0.15f * sinf(t * 6.0f))
                                : (0.65f + 0.35f * sinf(t * 6.0f));
        ImU32 ring = (kAccent & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24);
        float e = gap * k;
        dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e),
                    ring, r + e, 0, (s_padMode ? 4.0f : 3.0f) * k);
    }
    /* round X close button */
    bool CloseButton(ImVec2 c, float rad) const {
        int fi = Focus(ImVec2(c.x - rad, c.y - rad), ImVec2(c.x + rad, c.y + rad));
        bool hov = FHover(fi);
        dl->AddCircleFilled(c, rad, hov && down ? kCardDown : (hov ? kCardHov : kCard), 32);
        dl->AddCircle(c, rad, hov ? kAccent : kLine, 32,
                      hov ? (s_padMode ? 4.0f : 3.0f) * k : 1.2f);
        if (hov && s_padMode)
            dl->AddCircle(c, rad + 5.0f * k,
                          (kAccent & 0x00FFFFFF) | (70u << 24), 32, 3.0f * k);
        float h = rad * 0.42f;
        dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), kText, 3.0f * k);
        dl->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), kText, 3.0f * k);
        return FActivated(fi);
    }
};

/* ---- icons (monochrome, ImDrawList primitives) ---- */
static void IconPencil(const Ui& u, ImVec2 c, float sz) {
    /* pointing lower-left; u = along the pencil (towards eraser), n = across */
    float ux = 0.7071f, uy = -0.7071f, nx = 0.7071f, ny = 0.7071f;
    float L = sz * 0.95f, W = sz * 0.26f, T = sz * 0.28f;
    auto P = [&](float a, float b) { return ImVec2(c.x + ux * a + nx * b, c.y + uy * a + ny * b); };
    ImVec2 body[4] = { P(-L / 2, -W / 2), P(L / 2, -W / 2), P(L / 2, W / 2), P(-L / 2, W / 2) };
    u.dl->AddConvexPolyFilled(body, 4, IM_COL32(236, 236, 236, 255));
    /* eraser band + ferrule */
    ImVec2 band[4] = { P(L / 2 - W * 0.9f, -W / 2), P(L / 2, -W / 2), P(L / 2, W / 2), P(L / 2 - W * 0.9f, W / 2) };
    u.dl->AddConvexPolyFilled(band, 4, IM_COL32(240, 140, 160, 255));
    u.dl->AddLine(P(L / 2 - W * 0.9f, -W / 2), P(L / 2 - W * 0.9f, W / 2), kInk, 1.5f * u.k);
    /* ridge lines */
    u.dl->AddLine(P(-L / 2, -W / 6), P(L / 2 - W * 0.9f, -W / 6), IM_COL32(0, 0, 0, 50), 1.2f * u.k);
    u.dl->AddLine(P(-L / 2, W / 6), P(L / 2 - W * 0.9f, W / 6), IM_COL32(0, 0, 0, 50), 1.2f * u.k);
    /* tip */
    ImVec2 tip[3] = { P(-L / 2, -W / 2), P(-L / 2, W / 2), P(-L / 2 - T, 0) };
    u.dl->AddConvexPolyFilled(tip, 3, IM_COL32(230, 200, 150, 255));
    ImVec2 lead[3] = { P(-L / 2 - T * 0.55f, -W * 0.22f), P(-L / 2 - T * 0.55f, W * 0.22f), P(-L / 2 - T, 0) };
    u.dl->AddConvexPolyFilled(lead, 3, kInk);
    u.dl->AddPolyline(body, 4, kInk, ImDrawFlags_Closed, 1.5f * u.k);
    u.dl->AddPolyline(tip, 3, kInk, ImDrawFlags_Closed, 1.5f * u.k);
}

static void IconPerson(const Ui& u, ImVec2 c, float r, ImU32 fill, ImU32 outline) {
    /* head */
    u.dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.35f), r * 0.42f, fill, 32);
    u.dl->AddCircle(ImVec2(c.x, c.y - r * 0.35f), r * 0.42f, outline, 32, 1.6f * u.k);
    /* shoulders: top half-disc */
    ImVec2 sc(c.x, c.y + r * 0.95f);
    u.dl->PathArcTo(sc, r * 0.85f, 3.14159f, 6.28318f, 32);
    u.dl->PathFillConvex(fill);
    u.dl->PathArcTo(sc, r * 0.85f, 3.14159f, 6.28318f, 32);
    u.dl->PathStroke(outline, 0, 1.6f * u.k);
}

/* ---- lobby avatar: Project DIVA style hexagon ----
 * Pointy-top hexagon with a pink->orange diagonal gradient frame, glossy
 * upper-left bevel, white keyline and a pale plate holding the member's
 * Shared Set A module portrait, clipped to the inner hexagon. */
static const ImU32 kDivaPink   = IM_COL32(255, 64, 150, 255);
static const ImU32 kDivaOrange = IM_COL32(255, 154, 46, 255);

static ImU32 LerpCol(ImU32 a, ImU32 b, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    ImU32 o = 0;
    for (int s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        o |= (ImU32)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return o;
}

static void HexPts(ImVec2 c, float r, ImVec2 out[6], float rotDeg = -90.0f) {
    for (int i = 0; i < 6; i++) {
        float ang = (rotDeg + 60.0f * i) * 3.14159265f / 180.0f;
        out[i] = ImVec2(c.x + cosf(ang) * r, c.y + sinf(ang) * r);
    }
}

/* filled hexagon, c0 at the top-left -> c1 at the bottom-right; with a
 * texture, the square [uvC-uvR, uvC+uvR] maps to uv0..uv1 (tint = vertex
 * colour; uvC/uvR default to the hexagon centre/radius). rotDeg rotates
 * the vertex order (0 = flat-top, -90 = pointy-top). */
static void HexFill(ImDrawList* dl, ImVec2 c, float r, ImU32 c0, ImU32 c1,
                    ImTextureID tex = nullptr, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1),
                    float rotDeg = -90.0f, const ImVec2* uvC = nullptr, float uvR = 0.0f) {
    ImVec2 p[7];
    p[0] = c;
    HexPts(c, r, p + 1, rotDeg);
    const ImVec2 vc = uvC ? *uvC : c;
    const float vr = uvR > 0.0f ? uvR : r;
    if (tex) dl->PushTextureID(tex);
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(18, 7);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < 7; i++) {
        float t = ((p[i].x - c.x) + (p[i].y - c.y)) / (2.0f * 1.366f * r) + 0.5f;
        ImVec2 uv = tex ? ImVec2(uv0.x + (p[i].x - vc.x + vr) / (2.0f * vr) * (uv1.x - uv0.x),
                                 uv0.y + (p[i].y - vc.y + vr) / (2.0f * vr) * (uv1.y - uv0.y))
                        : white;
        dl->PrimWriteVtx(p[i], uv, LerpCol(c0, c1, t));
    }
    for (int i = 0; i < 6; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % 6));
    }
    if (tex) dl->PopTextureID();
}

static void Sparkle(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    ImVec2 v[4] = { ImVec2(c.x, c.y - s), ImVec2(c.x + s * 0.22f, c.y),
                    ImVec2(c.x, c.y + s), ImVec2(c.x - s * 0.22f, c.y) };
    ImVec2 h[4] = { ImVec2(c.x - s, c.y), ImVec2(c.x, c.y - s * 0.22f),
                    ImVec2(c.x + s, c.y), ImVec2(c.x, c.y + s * 0.22f) };
    dl->AddConvexPolyFilled(v, 4, col);
    dl->AddConvexPolyFilled(h, 4, col);
}

/* ---- platform icon, difficulty chip, slanted plates (shared by the room,
 * results and song-select draws) ---- */
/* which version of the mod a member runs: a Joy-Con pair for the Switch
 * game (Mega Mix), a monitor for the PC game (Mega Mix+). h tall, centred
 * on c; cut = the colour behind it (for the screen / stick cut-outs). */
static void IconPlat(ImDrawList* dl, ImVec2 c, float h, int plat, ImU32 col, ImU32 cut) {
    if (plat == PLAT_PC) {
        const float w = h * 1.22f, sh = h * 0.70f;
        ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y - h * 0.5f + sh);
        dl->AddRectFilled(a, b, col, h * 0.12f);
        const float in = h * 0.115f;
        dl->AddRectFilled(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), cut, h * 0.04f);
        dl->AddRectFilled(ImVec2(c.x - h * 0.09f, b.y), ImVec2(c.x + h * 0.09f, c.y + h * 0.40f), col);
        dl->AddRectFilled(ImVec2(c.x - h * 0.34f, c.y + h * 0.36f), ImVec2(c.x + h * 0.34f, c.y + h * 0.5f),
                          col, h * 0.06f);
    } else if (plat == PLAT_SWITCH) {
        const float w = h * 0.90f, gap = h * 0.09f, r = h * 0.24f;
        ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
        dl->AddRectFilled(a, ImVec2(c.x - gap * 0.5f, b.y), col, r, ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilled(ImVec2(c.x + gap * 0.5f, a.y), b, col, r, ImDrawFlags_RoundCornersRight);
        const float hw = (w - gap) * 0.5f;
        dl->AddCircleFilled(ImVec2(a.x + hw * 0.52f, a.y + h * 0.30f), h * 0.11f, cut, 12);
        dl->AddCircleFilled(ImVec2(b.x - hw * 0.52f, a.y + h * 0.62f), h * 0.11f, cut, 12);
    }
}
static float IconPlatW(float h, int plat) {
    return plat == PLAT_PC ? h * 1.22f : (plat == PLAT_SWITCH ? h * 0.90f : 0.0f);
}

/* parallelogram a..b leaning right by sk, vertical gradient top -> bot */
static void SlantPlate(ImDrawList* dl, ImVec2 a, ImVec2 b, float sk, ImU32 top, ImU32 bot) {
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(6, 4);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(ImVec2(a.x + sk, a.y), white, top);
    dl->PrimWriteVtx(ImVec2(b.x, a.y), white, top);
    dl->PrimWriteVtx(ImVec2(b.x - sk, b.y), white, bot);
    dl->PrimWriteVtx(ImVec2(a.x, b.y), white, bot);
    dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 1)); dl->PrimWriteIdx((ImDrawIdx)(base + 2));
    dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 2)); dl->PrimWriteIdx((ImDrawIdx)(base + 3));
}
static void SlantOutline(ImDrawList* dl, ImVec2 a, ImVec2 b, float sk, ImU32 col, float th) {
    ImVec2 q[4] = { ImVec2(a.x + sk, a.y), ImVec2(b.x, a.y), ImVec2(b.x - sk, b.y), ImVec2(a.x, b.y) };
    dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, th);
}
static ImU32 WithAlpha(ImU32 c, float a) {
    a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    return (c & 0x00FFFFFFu) | ((ImU32)(((c >> 24) & 0xFF) * a + 0.5f) << 24);
}

/* the game's difficulty names and colours (song select's bottom row) */
static const char* DiffLabel(int d, int e, bool compact = false) {
    static const char* N[] = { "EASY", "NORMAL", "HARD", "EXTREME" };
    if (d < 0 || d > 3) return "";
    return (d == 3 && e) ? (compact ? "EXEX" : "EX EXTREME") : N[d];
}
static void DiffCols(int d, int e, ImU32* top, ImU32* bot, ImU32* fg) {
    *fg = IM_COL32(255, 255, 255, 255);
    switch ((d == 3 && e) ? 4 : d) {
        case 0:  *top = IM_COL32(64, 208, 255, 255);  *bot = IM_COL32(0, 160, 236, 255);  break;
        case 1:  *top = IM_COL32(104, 228, 64, 255);  *bot = IM_COL32(40, 178, 28, 255);  break;
        case 2:  *top = IM_COL32(255, 212, 48, 255);  *bot = IM_COL32(246, 164, 0, 255);
                 *fg = IM_COL32(92, 56, 0, 255); break;
        case 3:  *top = IM_COL32(255, 88, 138, 255);  *bot = IM_COL32(234, 28, 96, 255);  break;
        default: *top = IM_COL32(196, 116, 252, 255); *bot = IM_COL32(132, 56, 212, 255); break;
    }
}
/* slanted difficulty tag, top-left at p, h tall; returns its width (0 when
 * the difficulty is unknown). draw = false only measures. */
static float DiffChip(ImDrawList* dl, ImFont* f, ImVec2 p, float h, int d, int e,
                      float alpha = 1.0f, bool draw = true, bool compact = false) {
    if (d < 0 || d > 3) return 0.0f;
    const char* s = DiffLabel(d, e, compact);
    const float fsz = h * 0.82f, sk = h * 0.30f, pad = h * 0.40f;
    ImVec2 ts = f->CalcTextSizeA(fsz, FLT_MAX, 0.0f, s);
    const float w = ts.x + pad * 2 + sk;
    if (!draw) return w;
    ImU32 top, bot, fg;
    DiffCols(d, e, &top, &bot, &fg);
    ImVec2 a = p, b(p.x + w, p.y + h);
    SlantPlate(dl, a, b, sk, WithAlpha(top, alpha), WithAlpha(bot, alpha));
    SlantOutline(dl, a, b, sk, IM_COL32(24, 14, 36, (int)(210 * alpha)), h * 0.09f);
    dl->AddText(f, fsz, ImVec2(p.x + pad + sk * 0.5f, p.y + (h - ts.y) * 0.5f), WithAlpha(fg, alpha), s);
    return w;
}

static void DrawAvatar(const Ui& u, ImVec2 c, float r, const mp::avatar::Ref& av, bool you, bool connected,
                       bool sparkle) {
    ImDrawList* dl = u.dl;
    ImVec2 p[6];
    if (you) {
        HexPts(c, r + 4.5f * u.k, p);
        dl->AddPolyline(p, 6, kAccent, ImDrawFlags_Closed, 3.0f * u.k);
    } else {
        HexFill(dl, ImVec2(c.x, c.y + 3.0f * u.k), r + 1.0f * u.k,
                IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90));
    }
    HexFill(dl, c, r, connected ? kDivaPink : IM_COL32(150, 150, 150, 255),
            connected ? kDivaOrange : IM_COL32(105, 105, 105, 255));
    HexPts(c, r - 1.6f * u.k, p);
    ImVec2 bevel[3] = { p[4], p[5], p[0] };
    dl->AddPolyline(bevel, 3, IM_COL32(255, 255, 255, 120), 0, 2.2f * u.k);

    float ri = r * 0.80f;
    HexFill(dl, c, ri + 2.2f * u.k, IM_COL32(255, 255, 255, 240), IM_COL32(255, 255, 255, 240));
    HexFill(dl, c, ri, connected ? IM_COL32(255, 230, 241, 255) : IM_COL32(200, 200, 200, 255),
            connected ? IM_COL32(255, 219, 196, 255) : IM_COL32(170, 170, 170, 255));

    void* tex = nullptr;
    float uv0[2], uv1[2];
    if (mp::avatar::Resolve(av, &tex, uv0, uv1) == 1) {
        HexFill(dl, c, ri, connected ? IM_COL32_WHITE : IM_COL32(140, 140, 140, 255),
                connected ? IM_COL32_WHITE : IM_COL32(140, 140, 140, 255),
                (ImTextureID)tex, ImVec2(uv0[0], uv0[1]), ImVec2(uv1[0], uv1[1]));
    } else {
        ImU32 sil = connected ? IM_COL32(255, 170, 200, 255) : IM_COL32(150, 150, 150, 255);
        IconPerson(u, ImVec2(c.x, c.y - ri * 0.08f), ri * 0.58f, sil, sil);
    }
    if (sparkle && connected) {
        HexPts(c, r, p);
        Sparkle(dl, ImVec2(p[1].x - r * 0.02f, p[1].y - r * 0.10f), r * 0.17f, IM_COL32(255, 255, 255, 235));
        Sparkle(dl, ImVec2(p[1].x + r * 0.12f, p[1].y + r * 0.16f), r * 0.08f, IM_COL32(255, 240, 200, 220));
    }
}

/* ================================================================
 * Results screen: top-3 avatars peeking in from the right edge.
 * Flat-top hexagons (rotDeg 0), gold for others / pink for "YOU",
 * NEW-RECORD-style glowing rank labels. All positions in 720p units
 * (x measured from the right edge W = DisplaySize.x).
 * ============================================================== */

/* multi-pass text: outer+inner pulsing glow, dark outline, vertical
 * gradient fill, then an italic shear applied to every pass. */
static void GlowText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                     const char* text, float alpha, float pulse, float k) {
    const int vtx0 = dl->VtxBuffer.Size;
    auto ring = [&](int n, float rad, ImU32 col) {
        for (int i = 0; i < n; i++) {
            float a = i * 6.28318f / n;
            dl->AddText(font, size,
                        ImVec2(pos.x + cosf(a) * rad * k, pos.y + sinf(a) * rad * k), col, text);
        }
    };
    /* sticker look like the game's NEW RECORD!: soft orange glow, dark red
     * rim, white edge, then the red-orange gradient fill */
    ring(16, 5.5f, IM_COL32(255, 170, 50, (int)(55 * alpha * pulse)));
    ring(16, 3.2f, IM_COL32(140, 16, 16, (int)(255 * alpha)));
    ring(12, 1.8f, IM_COL32(255, 255, 255, (int)(255 * alpha)));
    const int vtxFill = dl->VtxBuffer.Size;
    dl->AddText(font, size, pos, IM_COL32(255, 255, 255, (int)(255 * alpha)), text);
    const int vtxEnd = dl->VtxBuffer.Size;
    /* vertical gradient on the fill pass: (255,200,60) -> (255,52,40) */
    float yT = 1e30f, yB = -1e30f;
    for (int v = vtxFill; v < vtxEnd; v++) {
        yT = yT < dl->VtxBuffer[v].pos.y ? yT : dl->VtxBuffer[v].pos.y;
        yB = yB > dl->VtxBuffer[v].pos.y ? yB : dl->VtxBuffer[v].pos.y;
    }
    const ImU32 cT = IM_COL32(255, 200, 60, 255), cB = IM_COL32(255, 52, 40, 255);
    for (int v = vtxFill; v < vtxEnd; v++) {
        ImDrawVert& vt = dl->VtxBuffer[v];
        float t = yB > yT ? (vt.pos.y - yT) / (yB - yT) : 0.0f;
        vt.col = (LerpCol(cT, cB, t) & 0x00FFFFFFu) | (vt.col & 0xFF000000u);
    }
    /* italic shear on all passes */
    for (int v = vtx0; v < vtxEnd; v++)
        dl->VtxBuffer[v].pos.x += (yB - dl->VtxBuffer[v].pos.y) * 0.2f;
}

/* flat-top results hexagon: drop shadow, gold/pink frame, bevel, keyline,
 * pale plate, clipped portrait (uv box nudged down so the flat top edge
 * doesn't cut the head) or the silhouette fallback. */
static void DrawResultHex(const Ui& u, ImVec2 c, float r, const mp::avatar::Ref& av,
                          bool you, bool connected, bool final, bool sparkle,
                          float faceShift = 0.15f, bool plusSign = false) {
    ImDrawList* dl = u.dl;
    ImVec2 p[6];
    HexFill(dl, ImVec2(c.x, c.y + 3.0f * u.k), r + 1.0f * u.k,
            IM_COL32(0, 0, 0, 100), IM_COL32(0, 0, 0, 100), nullptr,
            ImVec2(0, 0), ImVec2(1, 1), 0.0f);
    ImU32 f0 = you ? IM_COL32(255, 110, 215, 255) : IM_COL32(255, 216, 72, 255);
    ImU32 f1 = you ? IM_COL32(226, 36, 158, 255) : IM_COL32(255, 138, 24, 255);
    if (!connected) { f0 = IM_COL32(150, 150, 150, 255);
                      f1 = IM_COL32(105, 105, 105, 255); }
    HexFill(dl, c, r, f0, f1, nullptr, ImVec2(0, 0), ImVec2(1, 1), 0.0f);
    HexPts(c, r - 1.6f * u.k, p, 0.0f);
    /* y grows down: verts 4/5 are the top ones */
    ImVec2 bevel[3] = { p[3], p[4], p[5] };         /* left, up-left, up-right */
    dl->AddPolyline(bevel, 3, IM_COL32(255, 255, 255, 130), 0, 2.2f * u.k);
    float ri = r * 0.86f;                            /* thin frame, big portrait */
    HexFill(dl, c, ri + 1.6f * u.k, IM_COL32(255, 255, 255, 240),
            IM_COL32(255, 255, 255, 240), nullptr, ImVec2(0, 0), ImVec2(1, 1), 0.0f);
    /* the right part is off-screen: centre faces in the visible part */
    const float vx = c.x - faceShift * ri;
    ImU32 pl0 = you ? IM_COL32(255, 222, 244, 255) : IM_COL32(255, 246, 222, 255);
    ImU32 pl1 = you ? IM_COL32(255, 236, 248, 255) : IM_COL32(255, 236, 200, 255);
    if (!connected) { pl0 = IM_COL32(200, 200, 200, 255);
                      pl1 = IM_COL32(170, 170, 170, 255); }
    HexFill(dl, c, ri, pl0, pl1, nullptr, ImVec2(0, 0), ImVec2(1, 1), 0.0f);
    void* tex = nullptr; float uv0[2], uv1[2];
    if (plusSign) {
        /* "and X others" slot of a 4+ tie: a bold plus instead of a face */
        const float t = ri * 0.24f, L = ri * 0.95f;
        const ImU32 pc = IM_COL32(232, 140, 30, 255);
        dl->AddRectFilled(ImVec2(vx - L * 0.5f, c.y - t * 0.5f),
                          ImVec2(vx + L * 0.5f, c.y + t * 0.5f), pc, t * 0.35f);
        dl->AddRectFilled(ImVec2(vx - t * 0.5f, c.y - L * 0.5f),
                          ImVec2(vx + t * 0.5f, c.y + L * 0.5f), pc, t * 0.35f);
    } else if (mp::avatar::Resolve(av, &tex, uv0, uv1) == 1) {
        ImU32 tint = !final ? IM_COL32(190, 190, 190, 255)
                   : (connected ? IM_COL32_WHITE : IM_COL32(140, 140, 140, 255));
        ImVec2 uvC(vx, c.y + 0.06f * ri);
        HexFill(dl, c, ri, tint, tint, (ImTextureID)tex,
                ImVec2(uv0[0], uv0[1]), ImVec2(uv1[0], uv1[1]), 0.0f, &uvC, ri);
    } else {
        ImU32 sil = !connected ? IM_COL32(150, 150, 150, 255)
                  : (you ? IM_COL32(255, 160, 215, 255)
                         : IM_COL32(240, 190, 120, 255));
        IconPerson(u, ImVec2(vx, c.y - ri * 0.05f), ri * 0.55f, sil, sil);
    }
    if (!final && !plusSign) {
        /* still playing: three pulsing dots on a dark pill, visible bottom;
         * scaled for the small cluster hexes */
        float ps = r / (50.0f * u.k);
        if (ps > 1.0f) ps = 1.0f;
        ImVec2 pc(vx, c.y + ri * 0.58f);
        dl->AddRectFilled(ImVec2(pc.x - 20 * u.k * ps, pc.y - 7 * u.k * ps),
                          ImVec2(pc.x + 20 * u.k * ps, pc.y + 7 * u.k * ps), IM_COL32(20, 16, 30, 190),
                          7 * u.k * ps);
        for (int i = 0; i < 3; i++) {
            float a = 0.35f + 0.65f *
                (0.5f + 0.5f * sinf(u.t * 6.0f - (float)i * 1.1f));
            ImU32 dc = you ? IM_COL32(255, 120, 220, (int)(255 * a))
                           : IM_COL32(255, 196, 70, (int)(255 * a));
            dl->AddCircleFilled(ImVec2(pc.x + (i - 1) * 11 * u.k * ps, pc.y), 3.6f * u.k * ps, dc, 12);
        }
    }
    if (sparkle && connected) {
        HexPts(c, r, p, 0.0f);
        /* top-left corner (vert 4) */
        Sparkle(dl, ImVec2(p[4].x - r * 0.04f, p[4].y + r * 0.02f), r * 0.20f,
                IM_COL32(255, 255, 255, 240));
        Sparkle(dl, ImVec2(p[4].x + r * 0.16f, p[4].y + r * 0.22f), r * 0.09f,
                IM_COL32(255, 240, 200, 225));
    }
}

/* the local player's own row follows the save immediately */
static mp::avatar::Ref LocalRef(int32_t local) {
    mp::avatar::Ref r;
    r.module = local;          /* own module: always this console's image */
    return r;
}
static mp::avatar::Ref StandingRef(const Snapshot& sn, int i) {
    int32_t local = mp::avatar::LocalModule();
    return (sn.standing[i].me && local >= 0) ? LocalRef(local) : sn.standing[i].av;
}

static float EaseOutBack(float p) {
    const float c1 = 1.70158f;
    float q = p - 1.0f;
    return 1.0f + (c1 + 1.0f) * q * q * q + c1 * q * q;
}

/* ================================================================
 * Expanded results panel: the hexagon column opens into one card per
 * player (rank, portrait, name, platform, chart, verdict, score, the
 * COOL/GOOD/SAFE/BAD/MISS split, max combo, %), like the web results.
 * resPerPage cards per page (4 in one column, 8 in two).
 * ============================================================== */
#ifdef _WIN32
static const char* const kResKeyA = "Enter";
static const char* const kResKeyY = "F7";
#else
static const char* const kResKeyA = "A";
static const char* const kResKeyY = "Y";
#endif
static float ResClamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float ResEase(float p) { p = ResClamp01(p); float q = 1.0f - p; return 1.0f - q * q * q; }
static ImU32 ResA(int r, int g, int b, float a) {
    return IM_COL32(r, g, b, (int)(255.0f * ResClamp01(a) + 0.5f));
}
static void ResFmtScore(int v, char* out, int n) {
    char t[16];
    snprintf(t, sizeof(t), "%d", v < 0 ? 0 : v);
    int len = (int)strlen(t), o = 0;
    for (int i = 0; i < len && o < n - 2; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = t[i];
    }
    out[o] = 0;
}
/* rounded key cap with a label; returns its width (draw = false measures) */
static float ResKeyCap(ImDrawList* dl, ImFont* f, ImVec2 p, float h, const char* label,
                       float a, bool draw = true) {
    const float fsz = h * 0.72f;
    ImVec2 ts = f->CalcTextSizeA(fsz, FLT_MAX, 0.0f, label);
    float w = ts.x + h * 0.7f;
    if (w < h) w = h;
    if (draw) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ResA(236, 240, 244, a), h * 0.5f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), ResA(28, 32, 36, a), h * 0.5f, 0, h * 0.08f);
        dl->AddText(f, fsz, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                    ResA(28, 32, 36, a), label);
    }
    return w;
}
/* "[key] text" pill, right edge at rx; used for the Details hint */
static void ResHint(ImDrawList* dl, ImFont* f, float rx, float y, float k, const char* key,
                    const char* text, float a) {
    const float h = 22.0f * k, fsz = 15.0f * k;
    ImVec2 ts = f->CalcTextSizeA(fsz, FLT_MAX, 0.0f, text);
    const float kw = ResKeyCap(dl, f, ImVec2(0, 0), h - 6.0f * k, key, 1.0f, false);
    const float w = 8.0f * k + kw + 6.0f * k + ts.x + 10.0f * k;
    ImVec2 p0(rx - w, y), p1(rx, y + h);
    dl->AddRectFilled(p0, p1, ResA(14, 20, 26, 0.84f * a), h * 0.5f);
    dl->AddRect(p0, p1, ResA(255, 255, 255, 0.22f * a), h * 0.5f, 0, 1.0f);
    ResKeyCap(dl, f, ImVec2(p0.x + 8.0f * k, y + 3.0f * k), h - 6.0f * k, key, a);
    dl->AddText(f, fsz, ImVec2(p0.x + 8.0f * k + kw + 6.0f * k, y + (h - ts.y) * 0.5f),
                ResA(255, 255, 255, a), text);
}

/* pp: 0 closed .. 1 open (drives the fly-out from the hexagon column);
 * inMs: ms since it opened (bar growth + score count-up) */
static void DrawResultsPanel(const Ui& u, const Snapshot& sn, float pp, float inMs, ImVec2 ds) {
    ImDrawList* dl = u.dl;
    const float k = u.k, W = ds.x / k;
    ImFont* bf = ImGui::GetFont();
    ImFont* big = FontLoader::Big() ? FontLoader::Big() : bf;
    const int per = sn.resPerPage > 0 ? sn.resPerPage : 4;
    const int cols = per > 4 ? 2 : 1, rowsN = 4;
    const int n = sn.standingCount;
    const int pages = (n + per - 1) / per;
    int page = sn.resPage;
    if (page >= pages) page = pages - 1;
    if (page < 0) page = 0;
    const float e0 = ResEase(pp);

    /* backdrop + panel */
    dl->AddRectFilled(ImVec2(0, 0), ds, ResA(4, 18, 24, 0.62f * e0));
    const float PX0 = (cols == 2 ? 46.0f : 176.0f), PX1 = W - PX0;
    const float PY0 = 70.0f, PY1 = 694.0f;
    const float grow = (1.0f - e0) * 26.0f;
    ImVec2 pa((PX0 + grow) * k, (PY0 + grow * 0.6f) * k), pb((PX1 - grow) * k, (PY1 - grow * 0.6f) * k);
    dl->AddRectFilled(ImVec2(pa.x + 4 * k, pa.y + 6 * k), ImVec2(pb.x + 4 * k, pb.y + 6 * k),
                      ResA(0, 0, 0, 0.35f * e0), 16 * k);
    dl->AddRectFilledMultiColor(pa, pb, ResA(22, 92, 96, 0.95f * e0), ResA(22, 92, 96, 0.95f * e0),
                                ResA(13, 54, 60, 0.97f * e0), ResA(13, 54, 60, 0.97f * e0));
    dl->AddRect(pa, pb, ResA(101, 255, 255, 0.8f * e0), 0.0f, 0, 2.0f * k);

    /* header: RESULTS + song, page n/m */
    {
        const float hy = (PY0 + 10.0f) * k;
        const float hs = 30.0f * k;
        dl->AddText(big, hs, ImVec2((PX0 + 22.0f) * k + 2 * k, hy + 2 * k), ResA(0, 60, 66, 0.9f * e0), "RESULTS");
        dl->AddText(big, hs, ImVec2((PX0 + 22.0f) * k, hy), ResA(255, 255, 255, e0), "RESULTS");
        const float tx = (PX0 + 22.0f) * k + big->CalcTextSizeA(hs, FLT_MAX, 0.0f, "RESULTS").x + 18 * k;
        char pg[24] = "";
        if (pages > 1) snprintf(pg, sizeof(pg), "%d / %d", page + 1, pages);
        ImVec2 ps = bf->CalcTextSizeA(18.0f * k, FLT_MAX, 0.0f, pg);
        if (pg[0])
            dl->AddText(bf, 18.0f * k, ImVec2((PX1 - 22.0f) * k - ps.x, hy + 8 * k), ResA(200, 236, 240, e0), pg);
        if (sn.selName[0]) {
            char nm[96];
            snprintf(nm, sizeof(nm), "%s", sn.selName);
            const float maxW = (PX1 - 22.0f) * k - ps.x - 16 * k - tx;
            int len = (int)strlen(nm);
            while (len > 0 && bf->CalcTextSizeA(18.0f * k, FLT_MAX, 0.0f, nm).x > maxW) {
                do { len--; } while (len > 0 && ((unsigned char)nm[len] & 0xC0) == 0x80);
                snprintf(nm + len, sizeof(nm) - len, "..");
            }
            dl->AddText(bf, 18.0f * k, ImVec2(tx, hy + 8 * k), ResA(169, 205, 211, e0), nm);
        }
        dl->AddLine(ImVec2((PX0 + 16.0f) * k, (PY0 + 52.0f) * k), ImVec2((PX1 - 16.0f) * k, (PY0 + 52.0f) * k),
                    ResA(101, 255, 255, 0.35f * e0), 1.5f * k);
    }

    /* cards */
    const float gap = 8.0f, colGap = 12.0f;
    const float cardsY0 = PY0 + 62.0f, cardsY1 = PY1 - 44.0f;
    const float cardH = (cardsY1 - cardsY0 - gap * (rowsN - 1)) / rowsN;
    const float cardW = (PX1 - PX0 - 36.0f - colGap * (cols - 1)) / cols;
    const float g = ResEase((inMs - 220.0f) / 650.0f);      /* bars + count-up */
    static const char* const JN[5] = { "COOL", "GOOD", "SAFE", "BAD", "MISS" };
    static const int JC[5][3] = { {255, 201, 51}, {67, 193, 234}, {67, 196, 85}, {63, 127, 232}, {178, 60, 240} };
    static const int JL[5][3] = { {255, 224, 138}, {155, 231, 255}, {141, 240, 154}, {143, 183, 255}, {226, 155, 255} };
    for (int j = 0; j < per; j++) {
        const int idx = page * per + j;
        if (idx >= n) break;
        const auto& st = sn.standing[idx];
        const int col = j / rowsN, row = j % rowsN;
        /* fly out of the hexagon column: every card starts as its hexagon */
        const float pj = ResEase(pp * 1.3f - j * 0.045f);
        const float ca = ResClamp01((pj - 0.35f) / 0.65f);       /* contents */
        const float fx0 = PX0 + 18.0f + col * (cardW + colGap), fy0 = cardsY0 + row * (cardH + gap);
        const float sx = W - 27.5f, sy = 307.0f + (idx < 3 ? idx : 3) * 116.0f;
        const float cxm = sx + (fx0 + cardW * 0.5f - sx) * pj, cym = sy + (fy0 + cardH * 0.5f - sy) * pj;
        const float cw = 96.0f + (cardW - 96.0f) * pj, chh = 88.0f + (cardH - 88.0f) * pj;
        ImVec2 a((cxm - cw * 0.5f) * k, (cym - chh * 0.5f) * k), b((cxm + cw * 0.5f) * k, (cym + chh * 0.5f) * k);
        const bool win = st.rank == 1 && st.final;
        /* plate */
        dl->AddRectFilled(a, b, st.me ? ResA(44, 108, 116, 0.96f * pj) : ResA(30, 78, 84, 0.94f * pj), 9 * k);
        if (win) {
            dl->AddRect(ImVec2(a.x - 2 * k, a.y - 2 * k), ImVec2(b.x + 2 * k, b.y + 2 * k),
                        ResA(255, 201, 51, 0.30f * pj), 11 * k, 0, 4.0f * k);
            dl->AddRect(a, b, ResA(255, 201, 51, pj), 9 * k, 0, 2.0f * k);
        } else {
            dl->AddRect(a, b, st.me ? ResA(119, 234, 241, 0.9f * pj) : ResA(101, 255, 255, 0.3f * pj),
                        9 * k, 0, (st.me ? 1.8f : 1.2f) * k);
        }
        /* portrait: travels from the column into the card */
        const float hr = 50.0f + (36.0f - 50.0f) * pj;
        const float hx = sx + (fx0 + 98.0f - sx) * pj, hy = sy + (fy0 + cardH * 0.5f - sy) * pj;
        DrawResultHex(u, ImVec2(hx * k, hy * k), hr * k, StandingRef(sn, idx), st.me, st.connected,
                      st.final, win && pj >= 1.0f, 0.0f, false);
        if (ca <= 0.0f) continue;
        /* rank */
        {
            char lab[16];
            snprintf(lab, sizeof(lab), "#%d", st.rank > 0 ? st.rank : idx + 1);
            const float ls = 30.0f * k;
            GlowText(dl, big, ls, ImVec2(a.x + 9 * k, (a.y + b.y) * 0.5f - ls * 0.55f), lab, ca, 1.0f, k);
        }
        const float x0 = a.x + 146.0f * k, xr = b.x - 14.0f * k;
        /* line 1: name, platform, chart, tags | verdict, score */
        const float y1 = a.y + 9.0f * k;
        char sc[20];
        ResFmtScore((int)(st.score * (st.final ? g : 1.0f) + 0.5f), sc, sizeof(sc));
        const float scs = 25.0f * k;
        ImVec2 scz = big->CalcTextSizeA(scs, FLT_MAX, 0.0f, sc);
        dl->AddText(big, scs, ImVec2(xr - scz.x + 1.5f * k, y1 + 1.5f * k), ResA(0, 40, 46, 0.8f * ca), sc);
        dl->AddText(big, scs, ImVec2(xr - scz.x, y1), ResA(255, 255, 255, ca), sc);
        const char* vt = ""; ImU32 vc = ResA(190, 204, 208, ca);
        if (!st.final) { vt = "PLAYING..."; vc = ResA(255, 201, 51, ca); }
        else if (st.drop) { vt = "DROP\xC3\x97OUT"; vc = ResA(255, 110, 196, ca); }
        else if (st.clear == 1 || (st.clear < 0 && st.border > 0.0f && st.pct >= st.border)) {
            vt = "STAGE CLEAR"; vc = ResA(119, 234, 241, ca); }
        else if (st.clear == 0 || (st.clear < 0 && st.border > 0.0f)) vt = "NOT CLEAR...";
        const float vs = 14.0f * k;
        ImVec2 vz = bf->CalcTextSizeA(vs, FLT_MAX, 0.0f, vt);
        const float vx = xr - scz.x - 12.0f * k - vz.x;
        dl->AddText(bf, vs, ImVec2(vx, y1 + 8.0f * k), vc, vt);
        {
            const float ns = 21.0f * k, th = 15.0f * k;
            const float iw = IconPlatW(th, st.plat);
            const float dw = DiffChip(dl, bf, ImVec2(0, 0), th, st.diff, st.ed, 1.0f, false, true);
            const float aw = st.autoplay ? bf->CalcTextSizeA(th * 0.8f, FLT_MAX, 0.0f, "AUTO").x + th * 0.9f : 0.0f;
            float extra = (iw > 0 ? iw + 8 * k : 0) + (dw > 0 ? dw + 6 * k : 0) + (aw > 0 ? aw + 6 * k : 0);
            char nm[64];
            snprintf(nm, sizeof(nm), "%s", st.me ? "YOU" : st.name);
            float maxW = vx - 10.0f * k - x0 - extra;
            if (maxW < 40.0f * k) maxW = 40.0f * k;
            int len = (int)strlen(nm);
            while (len > 0 && bf->CalcTextSizeA(ns, FLT_MAX, 0.0f, nm).x > maxW) {
                do { len--; } while (len > 0 && ((unsigned char)nm[len] & 0xC0) == 0x80);
                snprintf(nm + len, sizeof(nm) - len, "..");
            }
            ImVec2 nz = bf->CalcTextSizeA(ns, FLT_MAX, 0.0f, nm);
            dl->AddText(bf, ns, ImVec2(x0, y1 + 2.0f * k), ResA(255, 255, 255, ca), nm);
            float tx = x0 + nz.x + 8.0f * k;
            const float ty = y1 + 2.0f * k + (nz.y - th) * 0.5f;
            if (iw > 0) {
                IconPlat(dl, ImVec2(tx + iw * 0.5f, ty + th * 0.5f), th, st.plat, ResA(200, 236, 240, ca),
                         st.me ? ResA(44, 108, 116, ca) : ResA(30, 78, 84, ca));
                tx += iw + 8.0f * k;
            }
            if (dw > 0) { DiffChip(dl, bf, ImVec2(tx, ty), th, st.diff, st.ed, ca, true, true); tx += dw + 6.0f * k; }
            if (aw > 0 && tx + aw < vx - 4.0f * k) {
                SlantPlate(dl, ImVec2(tx, ty), ImVec2(tx + aw, ty + th), th * 0.3f, ResA(241, 80, 175, ca),
                           ResA(214, 52, 150, ca));
                dl->AddText(bf, th * 0.8f, ImVec2(tx + th * 0.5f, ty + th * 0.08f), ResA(255, 255, 255, ca), "AUTO");
            }
        }
        /* line 2: judgement split bar */
        int tot = 0;
        for (int q = 0; q < 5; q++) tot += st.jc[q];
        const float by0 = a.y + 45.0f * k, bh = 13.0f * k;
        dl->AddRectFilled(ImVec2(x0, by0), ImVec2(xr, by0 + bh), ResA(0, 0, 0, 0.32f * ca), 3 * k);
        if (st.hasJc && tot > 0) {
            float x = x0;
            const float full = (xr - x0) * g;
            for (int q = 0; q < 5; q++) {
                const float w = full * st.jc[q] / tot;
                if (w < 0.5f) continue;
                dl->AddRectFilledMultiColor(ImVec2(x, by0), ImVec2(x + w, by0 + bh),
                    ResA(JL[q][0], JL[q][1], JL[q][2], ca), ResA(JL[q][0], JL[q][1], JL[q][2], ca),
                    ResA(JC[q][0], JC[q][1], JC[q][2], ca), ResA(JC[q][0], JC[q][1], JC[q][2], ca));
                x += w;
            }
        }
        /* line 3: the five counts (shrunk to fit) */
        const float y3 = a.y + 65.0f * k;
        if (st.hasJc && tot > 0) {
            char vb[5][12];
            float js = 14.0f * k;
            for (int pass = 0; pass < 2; pass++) {
                float tw = 0.0f;
                for (int q = 0; q < 5; q++) {
                    snprintf(vb[q], sizeof(vb[q]), "%d", st.jc[q]);
                    tw += bf->CalcTextSizeA(js, FLT_MAX, 0.0f, JN[q]).x + js * 0.35f +
                          bf->CalcTextSizeA(js, FLT_MAX, 0.0f, vb[q]).x + (q < 4 ? js * 1.0f : 0.0f);
                }
                if (tw <= xr - x0 || pass) break;
                js *= (xr - x0) / tw;
            }
            float x = x0;
            for (int q = 0; q < 5; q++) {
                const float a2 = ca * ResClamp01((inMs - 420.0f - q * 60.0f) / 240.0f);
                dl->AddText(bf, js, ImVec2(x, y3), ResA(JL[q][0], JL[q][1], JL[q][2], a2), JN[q]);
                x += bf->CalcTextSizeA(js, FLT_MAX, 0.0f, JN[q]).x + js * 0.35f;
                dl->AddText(bf, js, ImVec2(x, y3), ResA(255, 255, 255, a2), vb[q]);
                x += bf->CalcTextSizeA(js, FLT_MAX, 0.0f, vb[q]).x + js * 1.0f;
            }
        } else {
            dl->AddText(bf, 14.0f * k, ImVec2(x0, y3), ResA(169, 205, 211, 0.8f * ca),
                        st.final ? "no note data" : "waiting for this player's result");
        }
        /* line 4: max combo, cool rate | % */
        const float y4 = a.y + 88.0f * k;
        {
            char pc[16];
            snprintf(pc, sizeof(pc), "%.2f%%", (double)(st.pct * (st.final ? g : 1.0f)));
            const float ps2 = 21.0f * k;
            ImVec2 pz = big->CalcTextSizeA(ps2, FLT_MAX, 0.0f, pc);
            dl->AddText(big, ps2, ImVec2(xr - pz.x, y4 - 3.0f * k), ResA(255, 255, 255, ca), pc);
            char l4[96];
            int o = snprintf(l4, sizeof(l4), "MAX COMBO %dx", st.max_combo);
            if (st.hasJc && tot > 0)
                o += snprintf(l4 + o, sizeof(l4) - o, "    COOL RATE %d%%", (int)(st.jc[0] * 100.0f / tot + 0.5f));
            if (st.border > 0.0f)
                snprintf(l4 + o, sizeof(l4) - o, "    CLEAR AT %.0f%%", (double)st.border);
            float fs4 = 13.5f * k;
            const float av = xr - pz.x - 12.0f * k - x0;
            const float w4 = bf->CalcTextSizeA(fs4, FLT_MAX, 0.0f, l4).x;
            if (w4 > av && w4 > 0.0f) fs4 *= av / w4;
            dl->AddText(bf, fs4, ImVec2(x0, y4 + 1.0f * k), ResA(200, 228, 232, ca), l4);
        }
    }

    /* footer: keys */
    {
        const float fy = (PY1 - 33.0f) * k, h = 22.0f * k, fsz = 15.0f * k;
        float x = (PX0 + 22.0f) * k;
        x += ResKeyCap(dl, bf, ImVec2(x, fy), h, kResKeyA, e0) + 6.0f * k;
        dl->AddText(bf, fsz, ImVec2(x, fy + 2.0f * k), ResA(255, 255, 255, e0), "Close");
        x += bf->CalcTextSizeA(fsz, FLT_MAX, 0.0f, "Close").x + 22.0f * k;
        if (pages > 1) {
            x += ResKeyCap(dl, bf, ImVec2(x, fy), h, "L", e0) + 4.0f * k;
            x += ResKeyCap(dl, bf, ImVec2(x, fy), h, "R", e0) + 6.0f * k;
            dl->AddText(bf, fsz, ImVec2(x, fy + 2.0f * k), ResA(255, 255, 255, e0), "Page");
        }
        const char* note = sn.standingsComplete ? "" : "Waiting for the other players...";
        ImVec2 nz = bf->CalcTextSizeA(fsz, FLT_MAX, 0.0f, note);
        dl->AddText(bf, fsz, ImVec2((PX1 - 22.0f) * k - nz.x, fy + 2.0f * k), ResA(255, 201, 51, e0), note);
    }
}

void DrawResultsAvatars() {
    Snapshot sn;
    GetSnapshot(&sn);
    if (sn.standingCount <= 0 || sn.resultsAgeMs < 0) return;
    const float age = sn.resultsAgeMs / 1000.0f;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    const float k = ds.y / 720.0f;

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ds, ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
    if (!BeginWin("##mpresults", nullptr, flags)) { EndWin(); return; }

    Ui u;
    u.dl = ImGui::GetWindowDrawList();
    ImFont* big = FontLoader::Big();
    u.font = big ? big : ImGui::GetFont();
    u.k = k;
    u.fs = u.font->FontSize;
    u.mouse = io.MousePos;
    u.clicked = false; u.down = false;
    u.t = (float)ImGui::GetTime();
    const float pulse = 0.8f + 0.2f * sinf(u.t * 6.28318f * 0.8f);
    /* expanded panel: 0 closed .. 1 open; the hexagon column hands over
     * to it while it is on screen */
    float pp = 0.0f;
    if (sn.resPanel == 2)
        pp = sn.resPanelMs < 0 ? 1.0f : ResClamp01(sn.resPanelMs / 420.0f);
    else if (sn.resPanel == 3 && sn.resPanelMs >= 0)
        pp = 1.0f - ResClamp01(sn.resPanelMs / 240.0f);

    /* rows are rank groups: consecutive standings sharing one rank.
     * first 3 groups, plus the local player's group if not among them */
    int gFirst[4], gCount[4], nRows = 0;
    for (int i = 0; i < sn.standingCount && nRows < 3; i++) {
        if (i > 0 && sn.standing[i].rank == sn.standing[i - 1].rank)
            continue;
        gFirst[nRows] = i;
        gCount[nRows] = 1;
        while (i + gCount[nRows] < sn.standingCount &&
               sn.standing[i + gCount[nRows]].rank == sn.standing[i].rank)
            gCount[nRows]++;
        nRows++;
    }
    /* difficulty tags only say something when the players weren't all on
     * the same chart (consoles that report none don't count) */
    bool mixedDiff = false;
    for (int i = 0, first = -1; i < sn.standingCount; i++) {
        if (sn.standing[i].diff < 0) continue;
        int key = sn.standing[i].diff * 2 + sn.standing[i].ed;
        if (first < 0) first = key;
        else if (key != first) mixedDiff = true;
    }
    int meRank = 0;
    for (int i = 0; i < sn.standingCount; i++)
        if (sn.standing[i].me) { meRank = sn.standing[i].rank; break; }
    bool meShown = false;
    for (int i = 0; i < nRows; i++)
        if (sn.standing[gFirst[i]].rank == meRank) meShown = true;
    if (!meShown && meRank > 0 && nRows < 4)
        for (int i = 0; i < sn.standingCount; i++)
            if (sn.standing[i].rank == meRank) {
                gFirst[nRows] = i;
                gCount[nRows] = 1;
                while (i + gCount[nRows] < sn.standingCount &&
                       sn.standing[i + gCount[nRows]].rank == meRank)
                    gCount[nRows]++;
                nRows++;
                break;
            }

    /* single players get one hex at r=50; a tie is one connected cluster of
     * about one hexagon's footprint (n=2: r=30 diagonal pair, n>=3: r=26
     * triangle; 4+ shows a "+" in the third slot) so it never reaches the
     * results table. Each row reserves one hexagon's height plus one line
     * per stacked name; rows below a tie move down and the column moves up
     * if it would run off the bottom. */
    const float HEXH = 90.0f, LINE = 16.0f;
    int lines[4]; float rowH[4], total = 0.0f;
    for (int i = 0; i < nRows; i++) {
        int c = gCount[i];
        lines[i] = c == 1 ? 1 : (c >= 4 ? 3 : c);
        rowH[i] = HEXH + 4.0f + lines[i] * LINE + 6.0f;
        total += rowH[i];
    }
    float top = 262.0f;
    if (top + total > 708.0f) top = 708.0f - total;
    if (top < 214.0f) top = 214.0f;
    for (int i = 0; i < nRows; i++) {
        int first = gFirst[i], cnt = gCount[i];
        int shown = cnt < 3 ? cnt : 3;
        int rank = sn.standing[first].rank;
        /* local player first, so YOU is never folded into "and X others" */
        int ord[8], no = 0;
        for (int m = 0; m < cnt && no < 8; m++)
            if (sn.standing[first + m].me) ord[no++] = first + m;
        for (int m = 0; m < cnt && no < 8; m++)
            if (!sn.standing[first + m].me) ord[no++] = first + m;
        const float rowTop = top;
        top += rowH[i];
        if (pp > 0.0f) continue;          /* the panel draws the players */
        float p = (age - (1.0f + 0.12f * i)) / 0.5f;
        if (p <= 0.0f) continue;
        if (p > 1.0f) p = 1.0f;
        float W = ds.x / k;
        float xoff = (1.0f - EaseOutBack(p)) * (2 * 50.0f + 60.0f);
        float alpha = p * p * (3.0f - 2.0f * p);          /* smoothstep */
        float cy = rowTop + HEXH * 0.5f;

        /* member center positions in 720p space */
        float r720 = 50.0f;
        ImVec2 hp[3];
        float bboxLeft, bboxCen;
        if (shown == 1) {
            hp[0] = ImVec2(W - 0.55f * 50.0f + xoff, cy);
            r720 = 50.0f;
            bboxLeft = hp[0].x - r720;
            bboxCen = hp[0].x - 0.12f * r720;
        } else if (shown == 2) {
            r720 = 30.0f;
            float ax = W + 0.15f * r720 - 2.5f * r720 + xoff;
            float ay = cy - 0.433f * r720;
            hp[0] = ImVec2(ax, ay);                    /* A upper-left */
            hp[1] = ImVec2(ax + 1.5f * r720, ay + 0.866f * r720); /* B */
            bboxLeft = ax - r720;
            bboxCen = ax + 0.75f * r720;
        } else {
            r720 = 26.0f;
            float ax = W + 0.15f * r720 - 2.5f * r720 + xoff;
            float ay = cy - 0.866f * r720;
            hp[0] = ImVec2(ax, ay);                    /* A upper-left */
            hp[1] = ImVec2(ax + 1.5f * r720, ay + 0.866f * r720); /* B */
            hp[2] = ImVec2(ax, ay + 1.732f * r720);    /* C below A */
            bboxLeft = ax - r720;
            bboxCen = ax + 0.75f * r720;
        }

        /* rank label, NEW RECORD! style; right edge at cluster left + 4,
         * same y across rows so the labels line up */
        char lab[12];
        snprintf(lab, sizeof(lab), "#%d", rank > 0 ? rank : first + 1);
        float lsz = 38.0f * k;
        ImVec2 lszV = u.font->CalcTextSizeA(lsz, FLT_MAX, 0.0f, lab);
        GlowText(u.dl, u.font, lsz,
                 ImVec2((bboxLeft + 4.0f) * k - lszV.x, (rowTop - 2.0f) * k),
                 lab, alpha, pulse, k);

        for (int m = 0; m < shown; m++) {
            const auto& st = sn.standing[ord[m]];
            ImVec2 c(hp[m].x * k, hp[m].y * k);
            DrawResultHex(u, c, r720 * k, StandingRef(sn, ord[m]), st.me,
                          st.connected, st.final,
                          i == 0 && m == 0,          /* top group sparkles */
                          shown > 1 ? 0.0f : 0.15f,
                          cnt >= 4 && m == 2);       /* "+" = the others */
        }
        /* the chart each player was on: a tag in the game's difficulty
         * colour sitting on the bottom edge of their hexagon (drawn after
         * the whole cluster so a neighbour never covers it) */
        if (mixedDiff) {
            const bool small = shown > 1;
            const float ch = (shown == 1 ? 14.0f : shown == 2 ? 11.5f : 10.5f) * k;
            ImFont* tf = ImGui::GetFont();
            for (int m = 0; m < shown; m++) {
                if (cnt >= 4 && m == 2) continue;     /* the "+" hexagon */
                const auto& st = sn.standing[ord[m]];
                const float cw = DiffChip(u.dl, tf, ImVec2(0, 0), ch, st.diff, st.ed, 1.0f, false, small);
                if (cw <= 0.0f) continue;
                float cx = hp[m].x * k - cw * 0.5f;
                if (cx + cw > ds.x - 3.0f * k) cx = ds.x - 3.0f * k - cw;
                DiffChip(u.dl, tf, ImVec2(cx, (hp[m].y + 0.866f * r720) * k - ch * 0.75f),
                         ch, st.diff, st.ed, alpha, true, small);
            }
        }

        /* names stacked one per line under the cluster (local reads YOU);
         * 4+: first, second, "and X others" */
        for (int L = 0; L < lines[i]; L++) {
            char nm[48];
            if (cnt >= 4 && L == 2)
                snprintf(nm, sizeof(nm), "and %d others", cnt - 2);
            else
                snprintf(nm, sizeof(nm), "%s",
                         sn.standing[ord[L]].me ? "YOU" : sn.standing[ord[L]].name);
            bool ascii = true;
            for (const char* s = nm; *s; s++) {
                unsigned char ch = (unsigned char)*s;
                if (ch > 126 || ch < 32) { ascii = false; break; }
            }
            ImFont* nf = ascii ? u.font : ImGui::GetFont();
            float nsz = 15.0f * k;
            float maxW = 140.0f * k;
            int len = (int)strlen(nm);
            while (nf->CalcTextSizeA(nsz, FLT_MAX, 0.0f, nm).x > maxW && len > 0) {
                do { len--; } while (len > 0 && ((unsigned char)nm[len] & 0xC0) == 0x80);
                snprintf(nm + len, sizeof(nm) - len, "..");
            }
            ImVec2 ntx = nf->CalcTextSizeA(nsz, FLT_MAX, 0.0f, nm);
            float nx = bboxCen * k - ntx.x * 0.5f;
            if (nx + ntx.x > ds.x - 6.0f * k) nx = ds.x - 6.0f * k - ntx.x;
            if (nx < 0) nx = 0;
            ImVec2 npos(nx, (rowTop + HEXH + 4.0f + L * LINE) * k);
            const ImU32 nOut = IM_COL32(30, 16, 48, (int)(230 * alpha));
            for (int d = 0; d < 8; d++) {
                float a = d * 6.28318f / 8.0f;
                u.dl->AddText(nf, nsz,
                    ImVec2(npos.x + cosf(a) * 1.2f * k, npos.y + sinf(a) * 1.2f * k), nOut, nm);
            }
            u.dl->AddText(nf, nsz, npos, IM_COL32(255, 255, 255, (int)(255 * alpha)), nm);
        }
    }
    if (pp > 0.0f) {
        Ui pu = u;
        pu.font = ImGui::GetFont();
        pu.fs = pu.font->FontSize;
        DrawResultsPanel(pu, sn, pp, sn.resPanel == 2 && sn.resPanelMs >= 0 ? (float)sn.resPanelMs : 99999.0f, ds);
    } else if (age > 1.6f && (sn.resPanel == 1 || (sn.resPanel == 3 && sn.resCanReopen))) {
        /* how to open it, above the first hexagon */
        const float ha = ResClamp01((age - 1.6f) / 0.4f) * (0.82f + 0.18f * pulse);
        ResHint(u.dl, ImGui::GetFont(), ds.x - 8.0f * k, 222.0f * k, k,
                sn.resPanel == 1 ? kResKeyA : kResKeyY, "Details", ha);
    }
    EndWin();
}

static void IconPeople(const Ui& u, ImVec2 c, float sz) {
    IconPerson(u, ImVec2(c.x + sz * 0.22f, c.y - sz * 0.12f), sz * 0.36f,
               IM_COL32(190, 190, 190, 255), kInk);
    IconPerson(u, ImVec2(c.x - sz * 0.18f, c.y + sz * 0.02f), sz * 0.46f,
               IM_COL32(240, 240, 240, 255), kInk);
}

static void IconShapes(const Ui& u, ImVec2 c, float sz) {
    /* Diva note shapes: triangle, circle, cross, square */
    float d = sz * 0.30f, r = sz * 0.19f, th = 4.0f * u.k;
    ImVec2 tc(c.x - d, c.y - d), cc(c.x + d, c.y - d), xc(c.x - d, c.y + d), sq(c.x + d, c.y + d);
    u.dl->AddTriangle(ImVec2(tc.x, tc.y - r * 1.05f), ImVec2(tc.x + r * 1.05f, tc.y + r * 0.8f),
                      ImVec2(tc.x - r * 1.05f, tc.y + r * 0.8f), IM_COL32(60, 220, 120, 255), th);
    u.dl->AddCircle(cc, r, IM_COL32(245, 85, 95, 255), 32, th);
    u.dl->AddLine(ImVec2(xc.x - r * 0.8f, xc.y - r * 0.8f), ImVec2(xc.x + r * 0.8f, xc.y + r * 0.8f), IM_COL32(100, 170, 255, 255), th);
    u.dl->AddLine(ImVec2(xc.x - r * 0.8f, xc.y + r * 0.8f), ImVec2(xc.x + r * 0.8f, xc.y - r * 0.8f), IM_COL32(100, 170, 255, 255), th);
    u.dl->AddRect(ImVec2(sq.x - r * 0.85f, sq.y - r * 0.85f), ImVec2(sq.x + r * 0.85f, sq.y + r * 0.85f),
                  IM_COL32(255, 125, 200, 255), 2.0f * u.k, 0, th);
}

static void IconGear(const Ui& u, ImVec2 c, float r, ImU32 col) {
    /* gear: 8 thick radial teeth + a ring + a centre hole */
    float tw = r * 0.42f;
    for (int i = 0; i < 8; i++) {
        float th = i * 3.14159265f / 4.0f;
        u.dl->AddLine(ImVec2(c.x + cosf(th) * r * 0.62f,
                             c.y + sinf(th) * r * 0.62f),
                      ImVec2(c.x + cosf(th) * r, c.y + sinf(th) * r),
                      col, tw);
    }
    u.dl->AddCircle(c, r * 0.66f, col, 24, tw * 0.8f);
    u.dl->AddCircle(c, r * 0.30f, col, 16, tw * 0.55f);
}

/* ---- helpers ---- */
static const char* MemberStatus(const MemberInfo& m) {
    if (!m.connected) return "disconnected";
    if (m.nosong) return "doesn't have this song";
    if (m.ready) return "ready";
    if (strcmp(m.reason, "modules") == 0) return "choosing modules";
    if (strcmp(m.reason, "wrong_song") == 0) return "wrong song";
    if (strcmp(m.reason, "difficulty") == 0) return "difficulty";
    if (strcmp(m.reason, "not_in_sel") == 0) return "not in song select";
    return "not ready";
}

static ImU32 MemberDot(const MemberInfo& m) {
    return !m.connected ? kDim : (m.nosong ? kRed : (m.ready ? kGreen : kYellow));
}

/* flat grey trophy (all three identical, Switch-menu style): a tall
 * rounded cup holding a large place number, short stem, flat base; h tall,
 * centred on c.x, top at c.y */
static void DrawTrophy(const Ui& u, ImVec2 c, float h, int place) {
    const ImU32 col = kMuted;
    float x = c.x, t = c.y, w = h * 0.40f;          /* cup half-width */
    float cupB = t + h * 0.74f;                     /* cup bottom */
    /* handles: half-ellipse loops on the cup's sides, from near the rim
     * down to about the cup's middle */
    for (int side = -1; side <= 1; side += 2) {
        ImVec2 hc(x + side * w, t + h * 0.31f);
        float rx = h * 0.15f, ry = h * 0.20f;
        for (int k = 0; k <= 12; k++) {
            float an = -1.5707963f + 3.1415926f * (float)k / 12.0f;
            u.dl->PathLineTo(ImVec2(hc.x + side * rx * cosf(an),
                                    hc.y + ry * sinf(an)));
        }
        u.dl->PathStroke(col, 0, h * 0.08f);
    }
    u.dl->AddRectFilled(ImVec2(x - w, t), ImVec2(x + w, cupB), col,
                        w * 0.85f, ImDrawFlags_RoundCornersBottom);
    u.dl->AddRectFilled(ImVec2(x - h * 0.07f, cupB - h * 0.02f),
                        ImVec2(x + h * 0.07f, t + h * 0.88f), col);
    u.dl->AddRectFilled(ImVec2(x - h * 0.26f, t + h * 0.88f),
                        ImVec2(x + h * 0.26f, t + h), col, h * 0.05f);
    char d[2] = { (char)('0' + place), 0 };
    float sc = h * 0.62f / u.Size("0", 1.0f).y;
    u.TextCentered(ImVec2(x - w, t), ImVec2(x + w, cupB - h * 0.04f), d, sc, kBg);
}

/* [1] n  [2] n  [3] n - times ranked 1st/2nd/3rd this room session; left
 * edge at a.x, vertically centred on a.y; gap = space after each count;
 * returns the width (draw=false only measures) */
static float DrawTrophyRow(const Ui& u, ImVec2 a, float h,
                           const int32_t places[3], bool draw = true,
                           float gap = 12.0f) {
    float x = a.x, sc = h * 0.70f / u.Size("0", 1.0f).y;
    float iw = h * 1.00f;                       /* icon box incl. handles */                       /* icon box incl. handles */
    for (int i = 0; i < 3; i++) {
        char n[8]; snprintf(n, sizeof(n), "%d", places[i]);
        ImVec2 ns = u.Size(n, sc);
        if (draw) {
            DrawTrophy(u, ImVec2(x + iw * 0.5f, a.y - h * 0.5f), h, i + 1);
            u.Text(ImVec2(x + iw + 4 * u.k, a.y - ns.y * 0.5f), n, sc,
                   places[i] ? kText : kDim);
        }
        x += iw + 3 * u.k + ns.x + (i < 2 ? gap * u.k : 0);
    }
    return x - a.x;
}

/* member indices in room session order (server position, join order
 * breaking ties and unknowns last) */
static int SessionOrder(const Snapshot& s, int* order) {
    int n = s.memberCount;
    for (int i = 0; i < n; i++) order[i] = i;
    auto key = [&](int i) { return s.members[i].srPos > 0 ? s.members[i].srPos : 99; };
    for (int i = 1; i < n; i++)          /* stable insertion sort */
        for (int j = i; j > 0 && key(order[j]) < key(order[j - 1]); j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    return n;
}

/* your own avatar follows the save immediately; others come from the server */
static mp::avatar::Ref MemberRef(const Snapshot& s, const MemberInfo& m) {
    int32_t local = mp::avatar::LocalModule();
    return (local >= 0 && strcmp(m.cid, s.myCid) == 0) ? LocalRef(local) : m.av;
}

static void GoHomeOrRoom() {
    /* the server address editor belongs to OPTIONS */
    s_screen = s_screen == SC_EDIT_SERVER ? SC_OPTIONS : InLobby() ? SC_ROOM : SC_HOME;
    s_kbArmed = false;
    if (VirtualKeyboard::IsOpen()) VirtualKeyboard::Close();
}

static void OnNameSubmit() {
    if (s_nameBuf[0]) CmdSetName(s_nameBuf);
    s_screen = SC_HOME; s_kbArmed = false;
}
static void OnCodeSubmit() {
    CmdLobbyJoin(s_codeBuf, true);
    memcpy(s_codeSent, s_codeBuf, sizeof(s_codeSent));
    s_joinShown = true;
    s_joinKbBack = true;
}
/* JOIN_ROOM's line next to the title: what became of the code as it was
 * submitted (until it is edited), else a warning while offline */
static void JoinNote(const Snapshot& s, char* out, int n, ImU32* col, bool* bad) {
    out[0] = 0; *col = kRed; *bad = false;
    bool mine = s_joinShown && strcmp(s_codeBuf, s_codeSent) == 0;
    if (mine && s.joinState == JOIN_PENDING) {
        snprintf(out, n, "Joining %s...", s.joinCode);
        *col = kMuted;
    } else if (mine && s.joinState == JOIN_FAILED) {
        snprintf(out, n, "%s", s.joinErr);
        *bad = true;
    } else if (!s.connected) {
        snprintf(out, n, "Not connected to the server");
    }
}

static void OpenEditName() {
    strncpy(s_nameBuf, Config::mpPlayerName.c_str(), sizeof(s_nameBuf) - 1);
    s_nameBuf[sizeof(s_nameBuf) - 1] = 0;
    s_screen = SC_EDIT_NAME;
    VirtualKeyboard::Open(s_nameBuf, sizeof(s_nameBuf), "SAVE", OnNameSubmit);
    s_kbArmed = true;
}
static void OnUrlSubmit() {
    if (!mp::SetCloudUrl(s_urlBuf))
        SystemBanner("Invalid server address - use host[:port][/path]");
    else if (mp::IsCloudMode())
        SystemBanner("Online server changed - connecting...");
    s_screen = SC_OPTIONS; s_kbArmed = false;
}
static void OpenEditServer() {
    snprintf(s_urlBuf, sizeof(s_urlBuf), "%s", mp::CloudUrl());
    s_screen = SC_EDIT_SERVER;
    VirtualKeyboard::Open(s_urlBuf, sizeof(s_urlBuf), "SAVE", OnUrlSubmit);
    s_kbArmed = true;
}
static void OpenJoinRoom() {
    s_codeBuf[0] = 0;
    s_joinShown = false;
    s_screen = SC_JOIN_ROOM;
    VirtualKeyboard::Open(s_codeBuf, sizeof(s_codeBuf), "JOIN", OnCodeSubmit);
    s_kbArmed = true;
}

/* header: title (left), optional X (right). Returns y of the divider. */
static float Header(const Ui& u, ImVec2 a, ImVec2 b, const char* title, bool withClose) {
    float hh = 66.0f * u.k;
    u.TextLeft(ImVec2(a.x + 30 * u.k, a.y), ImVec2(b.x - 100 * u.k, a.y + hh), title, 2.3f, kText);
    if (withClose && u.CloseButton(ImVec2(b.x - 42 * u.k, a.y + hh * 0.5f), 24 * u.k)) {
        sound::PlayCancelSE();
        SetOverlayOpen(false);
    }
    u.dl->AddLine(ImVec2(a.x + 24 * u.k, a.y + hh), ImVec2(b.x - 24 * u.k, a.y + hh), kLine, 1.5f);
    return a.y + hh;
}

static void Footer(const Ui& u, ImVec2 a, ImVec2 b, const char* hint, float reserveR = 0.0f,
                   ImU32 col = kMuted) {
    float fy = b.y - 44.0f * u.k;
    u.dl->AddLine(ImVec2(a.x + 24 * u.k, fy), ImVec2(b.x - 24 * u.k, fy), kLine, 1.5f);
    if (hint && *hint)
        u.TextLeft(ImVec2(a.x + 30 * u.k, fy), ImVec2(b.x - 30 * u.k - reserveR, b.y), hint, 1.3f, col);
}

/* ---- button guide glyphs (Switch style) ---- */
static void BtnGlyph(const Ui& u, ImVec2 c, float r, const char* label) {
    u.dl->AddCircleFilled(c, r, IM_COL32(232, 236, 240, 255), 24);
    u.dl->AddCircle(c, r, kInk, 24, 1.4f * u.k);
    ImVec2 ts = u.Size(label, 1.3f);
    u.dl->AddText(u.font, u.fs * 1.3f,
                  ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), kInk, label);
}
static void PillGlyph(const Ui& u, ImVec2 a, ImVec2 b, const char* label) {
    float r = (b.y - a.y) * 0.5f;
    u.dl->AddRectFilled(a, b, IM_COL32(232, 236, 240, 255), r);
    u.dl->AddRect(a, b, kInk, r, 0, 1.4f * u.k);
    ImVec2 ts = u.Size(label, 1.05f);
    u.dl->AddText(u.font, u.fs * 1.05f,
                  ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
                  kInk, label);
}
/* guide entries drawn right-aligned on the footer line */
struct GuideEnt { const char* glyph; const char* label; };
static float GuideW(const Ui& u, const GuideEnt* es, int n) {
    float w = 0, r = u.fs * 0.95f;
    for (int i = 0; i < n; i++) {
        bool pill = strlen(es[i].glyph) > 1;
        w += (pill ? r * 2.6f : r * 2.0f) + 6 * u.k +
             u.Size(es[i].label, 1.4f).x + 18 * u.k;
    }
    return w;
}
static void DrawGuide(const Ui& u, ImVec2 a, ImVec2 b, const GuideEnt* es, int n) {
    if (n <= 0) return;
    float r = u.fs * 0.95f;
    float x = b.x - 30 * u.k - GuideW(u, es, n);
    float cy = b.y - 22 * u.k;
    for (int i = 0; i < n; i++) {
        if (strlen(es[i].glyph) > 1) {
            float pw = r * 2.6f;
            PillGlyph(u, ImVec2(x, cy - r), ImVec2(x + pw, cy + r), es[i].glyph);
            x += pw;
        } else {
            BtnGlyph(u, ImVec2(x + r, cy), r, es[i].glyph);
            x += r * 2.0f;
        }
        ImVec2 ts = u.Size(es[i].label, 1.4f);
        u.dl->AddText(u.font, u.fs * 1.4f,
                      ImVec2(x + 6 * u.k, cy - ts.y * 0.5f), kMuted, es[i].label);
        x += 6 * u.k + ts.x + 18 * u.k;
    }
}
/* the guide sits on the footer line; reserve its width for Footer() */
static float GuideReserve(const Ui& u, const GuideEnt* es, int n) {
    return n > 0 ? GuideW(u, es, n) + 10 * u.k : 0.0f;
}

/* ---------------------------------------------------------------- HOME */
static void DrawHome(const Ui& u, const Snapshot& s, ImVec2 a, ImVec2 b) {
    float top = Header(u, a, b, "Multiplayer", true);
    bool online = s.connected;
    mp::MpStatus ns;
    mp::GetStatus(&ns);

    /* footer pill: MULTIPLAYER: state (+ which server) */
    const bool mpOff = !Config::mpEnabled || ns.mode == mp::kServerOff;
    const char* st = mpOff ? "MULTIPLAYER: OFF"
                   : !online ? "MULTIPLAYER: DISCONNECTED"
                   : ns.cloud ? "MULTIPLAYER: CONNECTED (ONLINE)"
                              : "MULTIPLAYER: CONNECTED (LAN)";
    ImU32 stc = mpOff ? kDim : (online ? kGreen : kRed);
    float pillH = 40 * u.k;
    ImVec2 ps = u.Size(st, 1.3f);
    float pw = ps.x + 44 * u.k;
    ImVec2 pa((a.x + b.x - pw) * 0.5f, b.y - 24 * u.k - pillH), pb(pa.x + pw, pa.y + pillH);
    u.dl->AddRectFilled(pa, pb, (stc & 0x00FFFFFF) | (40u << 24), pillH * 0.5f);
    u.dl->AddRect(pa, pb, stc, pillH * 0.5f, 0, 1.4f);
    u.TextCentered(pa, pb, st, 1.3f, stc);

    /* offline tip, centred between the cards and the pill */
    char tipBuf[160];
    const char* tip = !Config::mpEnabled ? "Set enabled = true in config.toml, then restart"
                    : mpOff ? "Multiplayer is off - pick LOCAL or ONLINE in Options"
                    : (!online ? "Looking for a server on this network..." : nullptr);
    /* the wake-up wait only exists for the online (free-tier) server */
    if (!mpOff && !online && ns.mode == mp::kServerCloud) {
        if (ns.state == mp::MpState::Error && ns.last_error[0])
            snprintf(tipBuf, sizeof(tipBuf), "Can't reach %s (%s) - retrying...",
                     mp::CloudUrl(), ns.last_error);
        else
            snprintf(tipBuf, sizeof(tipBuf),
                     "Connecting to %s (waking server, up to 60s)...", mp::CloudUrl());
        tip = tipBuf;
    }

    /* three cards */
    float gap = 22 * u.k;
    float cy0 = top + 24 * u.k, cy1 = pa.y - 22 * u.k - (tip ? 30 * u.k : 0);
    float cw = (b.x - a.x - 2 * 30 * u.k - 2 * gap) / 3.0f;
    struct Card { const char* label; bool enabled; } cards[3] = {
        { "EDIT NAME", true }, { "CREATE ROOM", online }, { "JOIN ROOM", online } };
    for (int i = 0; i < 3; i++) {
        ImVec2 ca(a.x + 30 * u.k + i * (cw + gap), cy0), cb(ca.x + cw, cy1);
        int fi = u.Focus(ca, cb, cards[i].enabled);
        bool hov = cards[i].enabled && u.FHover(fi);
        u.dl->AddRectFilled(ca, cb, hov ? (u.down ? kCardDown : kCardHov) : kCard, 16 * u.k);
        if (hov) {
            u.Ring(ca, cb, 16 * u.k);
        } else {
            u.dl->AddRect(ca, cb, kLine, 16 * u.k, 0, 1.4f);
        }
        float ih = (cb.y - ca.y);
        ImVec2 ic((ca.x + cb.x) * 0.5f, ca.y + ih * 0.42f);
        float isz = ih * 0.42f;
        if (i == 0) IconPencil(u, ic, isz);
        else if (i == 1) IconPeople(u, ic, isz);
        else IconShapes(u, ic, isz);
        if (!cards[i].enabled)
            u.dl->AddRectFilled(ca, cb, IM_COL32(45, 45, 45, 150), 16 * u.k);
        u.TextCentered(ImVec2(ca.x, cb.y - ih * 0.30f), ImVec2(cb.x, cb.y - ih * 0.08f),
                       cards[i].label, 1.9f, cards[i].enabled ? kText : kDim);
        if (u.FActivated(fi) && !cards[i].enabled) {
            /* a touch lands on a greyed-out card too: say why it is off */
            sound::PlayCancelSE();
            SystemBanner(mpOff ? "Multiplayer is off"
                               : "Not connected to a server yet");
        } else if (u.FActivated(fi)) {
            sound::PlayEnterSE();
            if (i == 0) OpenEditName();
            else if (i == 1) CmdLobbyCreate();
            else OpenJoinRoom();
        }
    }
    /* current name, left of the state pill */
    char nm[64];
    snprintf(nm, sizeof(nm), "Name: %s", Config::mpPlayerName.c_str());
    u.TextLeft(ImVec2(a.x + 30 * u.k, pa.y), ImVec2(pa.x - 10 * u.k, pb.y), nm, 1.3f, kMuted);
    /* Options pill-button, right of the state pill; focused AFTER the
     * three cards so the focus order is X, EDIT, CREATE, JOIN, OPTIONS */
    {
        float ow = 130 * u.k;
        ImVec2 oa(pb.x + 12 * u.k, pa.y), ob(oa.x + ow, pb.y);
        int fi = u.Focus(oa, ob, true);
        bool hov = u.FHover(fi);
        u.dl->AddRectFilled(oa, ob,
            hov ? (u.down ? kCardDown : kCardHov) : kCard, pillH * 0.5f);
        if (hov) u.Ring(oa, ob, pillH * 0.5f);
        else u.dl->AddRect(oa, ob, kLine, pillH * 0.5f, 0, 1.4f);
        ImVec2 gc(oa.x + 24 * u.k, (oa.y + ob.y) * 0.5f);
        IconGear(u, gc, 11 * u.k, kText);
        u.TextCentered(ImVec2(gc.x + 10 * u.k, oa.y), ob, "Options",
                       1.3f, kText);
        if (u.FActivated(fi)) {
            sound::PlayEnterSE();
            s_screen = SC_OPTIONS;
        }
    }
    /* offline tip, centred in the band the cards made room for */
    if (tip) {
        char tb[96];
        u.Fit(tip, b.x - a.x - 60 * u.k, 1.15f, tb, sizeof(tb));
        ImVec2 tsa(a.x + 30 * u.k, cy1 + 6 * u.k), tsb(b.x - 30 * u.k, pa.y - 8 * u.k);
        u.TextCentered(tsa, tsb, tb, 1.15f, kMuted);
    }
    /* pad button guide, bottom-right of the content area */
    const GuideEnt g[] = {{"A", "OK"}, {"B", "Close"}};
    float gw = GuideW(u, g, 2);
    ImVec2 ga(b.x - 30 * u.k - gw - 10 * u.k, a.y);
    DrawGuide(u, ga, b, g, 2);
}

/* -------------------------------------------------- EDIT NAME / JOIN */
static void DrawTextScreen(const Ui& u, ImVec2 a, ImVec2 b, const char* title,
                           const char* buf, const char* okLabel, bool upper,
                           void (*onOk)(), const char* help, bool allowEmpty = false,
                           const char* note = nullptr, ImU32 noteCol = kMuted,
                           bool bad = false) {
    float top = Header(u, a, b, title, false);
    /* what the screen's action is doing / why it was refused, right of
     * the title */
    if (note && *note) {
        float x0 = a.x + 30 * u.k + u.Size(title, 2.3f).x + 36 * u.k;
        char nb[96];
        u.Fit(note, b.x - 30 * u.k - x0, 1.55f, nb, sizeof(nb));
        ImVec2 ns = u.Size(nb, 1.55f);
        u.Text(ImVec2(b.x - 30 * u.k - ns.x, a.y + (top - a.y - ns.y) * 0.5f),
               nb, 1.55f, noteCol);
    }

    /* field */
    float fh = 70 * u.k;
    ImVec2 fa(a.x + 30 * u.k, top + 28 * u.k), fb(b.x - 30 * u.k, fa.y + fh);
    u.dl->AddRectFilled(fa, fb, kField, 10 * u.k);
    u.dl->AddRect(fa, fb, bad ? kRed : kAccent, 10 * u.k, 0, 2.0f * u.k);
    char shown[104];
    if (upper) {
        int i = 0;
        for (; buf[i] && i < (int)sizeof(shown) - 1; i++) {
            char ch = buf[i];
            shown[i] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
        }
        shown[i] = 0;
    } else {
        strncpy(shown, buf, sizeof(shown) - 1); shown[sizeof(shown) - 1] = 0;
    }
    float tscale = upper ? 2.6f : 2.0f;
    ImVec2 ts = u.Size(shown, tscale);
    float tx = upper ? (fa.x + fb.x - ts.x) * 0.5f : fa.x + 18 * u.k;
    float ty = fa.y + (fh - ts.y) * 0.5f;
    u.Text(ImVec2(tx, ty), shown, tscale, kText);
    /* caret */
    int cp = VirtualKeyboard::GetCursorPos();
    if (cp > (int)strlen(shown)) cp = (int)strlen(shown);
    char pre[104]; memcpy(pre, shown, cp); pre[cp] = 0;
    float cx = tx + u.Size(pre, tscale).x + 1.0f;
    if (!buf[0] && help) {
        /* placeholder, with the caret parked just left of it */
        ImVec2 hs = u.Size(help, 1.45f);
        float hx = upper ? (fa.x + fb.x - hs.x) * 0.5f : fa.x + 18 * u.k + 6 * u.k;
        u.Text(ImVec2(hx, fa.y + (fh - hs.y) * 0.5f), help, 1.45f, kDim);
        cx = hx - 6 * u.k;
    }
    if (fmodf(u.t, 1.0f) < 0.6f)
        u.dl->AddLine(ImVec2(cx, ty), ImVec2(cx, ty + ts.y), kAccent, 2.0f * u.k);

    /* CANCEL / OK */
    float bh = 60 * u.k, gap = 24 * u.k;
    float bw = (fb.x - fa.x - gap) * 0.5f;
    ImVec2 ca(fa.x, fb.y + 22 * u.k), cb(ca.x + bw, ca.y + bh);
    ImVec2 oa(cb.x + gap, ca.y), ob(oa.x + bw, cb.y);
    if (u.Button(ca, cb, "CANCEL", Ui::B_NORMAL, 1.55f, true, false)) {
        sound::PlayCancelSE();
        GoHomeOrRoom();
    }
    if (u.Button(oa, ob, okLabel, Ui::B_PRIMARY, 1.55f, buf[0] != 0 || allowEmpty, false)) {
        sound::PlayEnterSE();
        if (VirtualKeyboard::IsOpen()) VirtualKeyboard::Close();
        onOk();
    }
    /* pad guide for the on-screen keyboard */
    const GuideEnt g[] = {{"A", "Type"}, {"B", "Delete"}, {"Y", "Space"},
                          {"X", "Caps"}, {"+", okLabel}};
    DrawGuide(u, ImVec2(a.x, cb.y), b, g, 5);
}

/* ---------------------------------------------------------------- ROOM */
static const char* RoleCaption(const Snapshot& s, const MemberInfo& m, char* out, int n) {
    bool lead = strcmp(m.cid, s.leaderCid) == 0;
    bool own  = strcmp(m.cid, s.ownerCid) == 0;
    bool you  = strcmp(m.cid, s.myCid) == 0;
    out[0] = 0;
    int len = 0;
    auto add = [&](const char* w) {
        len += snprintf(out + len, n - len, "%s%s", len ? ", " : "", w);
    };
    if (own)  add("Owner");
    if (lead) add("Leader");
    if (you)  add("You");
    if (len) {
        char tmp[48]; snprintf(tmp, sizeof(tmp), "(%s)", out);
        strncpy(out, tmp, n - 1); out[n - 1] = 0;
    }
    return out;
}

static void DrawRoom(const Ui& u, const Snapshot& s, ImVec2 a, ImVec2 b) {
    char title[32];
    snprintf(title, sizeof(title), "ROOM %s", s.code);
    float top = Header(u, a, b, title, true);
    /* offline: owner actions can't reach the server, so hide them */
    bool owner = strcmp(s.ownerCid, s.myCid) == 0 && s.connected;

    /* header buttons: [Room Config] [Exit Room]  (X) */
    float bh = 48 * u.k, bw = 190 * u.k, gap = 14 * u.k;
    float bx = b.x - 42 * u.k - 24 * u.k - 22 * u.k;   /* left of the X */
    float by = a.y + (66 * u.k - bh) * 0.5f;
    ImVec2 ea(bx - bw, by), eb(bx, by + bh);
    /* Exit Room: pad needs a 2-step confirm, touch stays single-tap */
    int fiExit = u.Focus(ea, eb, true, true);
    bool ehov = u.FHover(fiExit);
    u.dl->AddRectFilled(ea, eb, (ehov || s_exitArmed) ? kRed : kRedBg, 10 * u.k);
    u.dl->AddRect(ea, eb, kRed, 10 * u.k, 0, (ehov || s_exitArmed) ? 2.0f : 1.2f);
    if (ehov && s_padMode) u.Ring(ea, eb, 10 * u.k);
    u.TextCentered(ea, eb, s_exitArmed ? "Press A to leave" : "Exit Room",
                   1.4f, (ehov || s_exitArmed) ? kInk : kRed);
    if (u.FPadActivated(fiExit)) {
        if (s_exitArmed) {
            s_exitArmed = false;
            sound::PlayCancelSE();
            CmdLobbyLeave();
        } else {
            s_exitArmed = true;
            s_exitArmUntil = NavNowMs() + 3000;
            sound::PlaySelectSE();
        }
    } else if (u.FActivated(fiExit)) {     /* touch single-tap */
        sound::PlayCancelSE();
        CmdLobbyLeave();
    }
    if (owner) {
        ImVec2 ra(ea.x - gap - bw, by), rb(ea.x - gap, by + bh);
        if (u.Button(ra, rb, "Room Config", Ui::B_NORMAL, 1.4f)) {
            sound::PlayEnterSE();
            s_screen = SC_ROOM_CONFIG;
        }
    }

    float cy0 = top + 16 * u.k;
    float cy1 = b.y - 44 * u.k;
    float splitX = a.x + (b.x - a.x) * 0.54f;

    /* ---- podium: the top three of the room session ranking (wins,
     * then placement score; join order until a round has counted) ---- */
    int order[8];
    SessionOrder(s, order);
    int n = s.memberCount < 3 ? s.memberCount : 3;
    float colW = (splitX - a.x - 30 * u.k) / 3.0f;
    float rads[3]  = { 76 * u.k, 63 * u.k, 54 * u.k };
    float plateW[3] = { 70 * u.k, 59 * u.k, 52 * u.k };
    /* avatar bottoms align here; the whole podium block (~2r + plate +
     * name) sits around the vertical middle of the content area */
    float baseY = (cy0 + cy1) * 0.5f + 12 * u.k;
    for (int i = 0; i < n; i++) {
        const MemberInfo& m = s.members[order[i]];
        float cx = a.x + 30 * u.k + colW * (i + 0.5f);
        float r = rads[i];
        ImVec2 ac(cx, baseY - r);
        bool you = strcmp(m.cid, s.myCid) == 0;
        DrawAvatar(u, ac, r, MemberRef(s, m), you, m.connected, false);
        /* status dot on the lower-right edge */
        float dr = 9 * u.k;
        ImVec2 dc(ac.x + r * 0.64f, ac.y + r * 0.68f);
        u.dl->AddCircleFilled(dc, dr, MemberDot(m), 20);
        u.dl->AddCircle(dc, dr, kBg, 20, 2.5f * u.k);
        /* numbered plate */
        float pw = plateW[i], ph = pw * 0.9f;
        ImVec2 pa(cx - pw * 0.5f, baseY + 12 * u.k), pb(pa.x + pw, pa.y + ph);
        u.dl->AddRectFilled(pa, pb, IM_COL32(236, 236, 236, 255), 7 * u.k);
        u.dl->AddRect(pa, pb, kInk, 7 * u.k, 0, 2.0f * u.k);
        char num[4]; snprintf(num, sizeof(num), "%d", i + 1);
        u.TextCentered(pa, pb, num, i == 0 ? 2.4f : 1.9f, kInk);
        /* name + caption */
        float ny = pb.y + 10 * u.k;
        float nscale = i == 0 ? 1.7f : 1.5f;
        char nm[64];
        const float pih = (i == 0 ? 19 : 17) * u.k, piw = IconPlatW(pih, m.plat);
        const float pig = piw > 0 ? piw + 9 * u.k : 0.0f;
        u.Fit(m.name, colW - 6 * u.k - pig, nscale, nm, sizeof(nm));
        ImVec2 nsz = u.Size(nm, nscale);
        const float nx0 = cx - (nsz.x + pig) * 0.5f;
        u.Text(ImVec2(nx0, ny), nm, nscale, m.connected ? kText : kDim);
        if (piw > 0)
            IconPlat(u.dl, ImVec2(nx0 + nsz.x + 9 * u.k + piw * 0.5f, ny + nsz.y * 0.5f + 1 * u.k),
                     pih, m.plat, m.connected ? kMuted : kDim, kBg);
        char cap[48];
        RoleCaption(s, m, cap, sizeof(cap));
        float ty = ny + nsz.y + 6 * u.k;
        if (cap[0]) {
            ImVec2 csz = u.Size(cap, 1.2f);
            u.Text(ImVec2(cx - csz.x * 0.5f, ny + nsz.y + 3 * u.k), cap, 1.2f, kMuted);
            ty += csz.y + 2 * u.k;
        }
        float th = 20 * u.k;
        float tw = DrawTrophyRow(u, ImVec2(0, 0), th, m.places, false);
        DrawTrophyRow(u, ImVec2(cx - tw * 0.5f, ty + 4 * u.k + th * 0.5f), th,
                      m.places);
    }
    if (n == 0)
        u.TextCentered(ImVec2(a.x, cy0), ImVec2(splitX, cy1), "Waiting for the room list...", 1.45f, kMuted);

    /* ---- ordered list with kick buttons ---- */
    float lx = splitX + 10 * u.k;
    float rowH = 56 * u.k;
    float ly = cy0 + 6 * u.k;
    bool assignMode = owner && strcmp(s.pickMode, "assigned") == 0;
    for (int i = 0; i < s.memberCount; i++) {
        const MemberInfo& m = s.members[order[i]];
        bool you = strcmp(m.cid, s.myCid) == 0;
        bool lead = strcmp(m.cid, s.leaderCid) == 0;
        ImVec2 ra(lx, ly + i * rowH), rb(b.x - 30 * u.k, ra.y + rowH - 6 * u.k);
        if (ra.y + rowH > cy1) break;
        bool assignable = assignMode && !lead;
        int fiRow = u.Focus(ra, ImVec2(rb.x - 44 * u.k, rb.y), assignable);
        bool rowHov = assignable && u.FHover(fiRow);
        bool armed = you == false && s_armCid[0] && strcmp(s_armCid, m.cid) == 0;
        if (armed)
            u.dl->AddRectFilled(ra, rb, kRedBg, 8 * u.k);
        if (you || rowHov)
            u.dl->AddRectFilled(ra, rb, rowHov ? kCardHov : kAccentBg, 8 * u.k);
        if (rowHov && s_padMode)
            u.Ring(ra, rb, 8 * u.k);
        char idx[6]; snprintf(idx, sizeof(idx), "%d.", i + 1);
        u.TextLeft(ImVec2(ra.x + 12 * u.k, ra.y), ImVec2(ra.x + 40 * u.k, rb.y), idx, 1.5f, kMuted);
        /* small hex avatar, status dot on its lower-right edge */
        ImVec2 hc(ra.x + 62 * u.k, (ra.y + rb.y) * 0.5f);
        float hr = 21 * u.k;
        DrawAvatar(u, hc, hr, MemberRef(s, m), false, m.connected, false);
        ImVec2 dc(hc.x + hr * 0.66f, hc.y + hr * 0.66f);
        u.dl->AddCircleFilled(dc, 6 * u.k, MemberDot(m), 16);
        u.dl->AddCircle(dc, 6 * u.k, kBg, 16, 2.0f * u.k);
        float nameX = ra.x + 94 * u.k;
        float nameR = rb.x - (owner && !you ? 64 * u.k : 8 * u.k);
        /* session trophies, right-aligned before the kick button */
        {
            float th = 20 * u.k;
            float tw = DrawTrophyRow(u, ImVec2(0, 0), th, m.places, false, 8.0f);
            DrawTrophyRow(u, ImVec2(nameR - tw, (ra.y + rb.y) * 0.5f),
                          th, m.places, true, 8.0f);
            nameR -= tw + 12 * u.k;
        }
        /* name, then the platform icon (Switch / PC version of the mod)
         * and the LEADER / NO SONG tags */
        {
            const float my = (ra.y + rb.y) * 0.5f;
            const float ih = 18 * u.k, iw = IconPlatW(ih, m.plat);
            char nmb[64];
            u.Fit(m.name, nameR - nameX - (iw > 0 ? iw + 10 * u.k : 0.0f), 1.6f, nmb, sizeof(nmb));
            ImVec2 nsz = u.Size(nmb, 1.6f);
            u.Text(ImVec2(nameX, my - nsz.y * 0.5f), nmb, 1.6f, m.connected ? kText : kDim);
            float tx = nameX + nsz.x + 10 * u.k;
            if (iw > 0) {
                /* the colour under the icon, for its cut-outs */
                ImU32 cut = rowHov ? kCardHov
                          : you ? IM_COL32(38, 69, 74, 255)
                          : armed ? IM_COL32(79, 52, 54, 255) : IM_COL32(45, 45, 45, 255);
                IconPlat(u.dl, ImVec2(tx + iw * 0.5f, my + 1 * u.k), ih, m.plat,
                         m.connected ? kMuted : kDim, cut);
                tx += iw + 10 * u.k;
            }
            auto tag = [&](const char* s, ImU32 fg, ImU32 bg) {
                float tw = u.Size(s, 0.95f).x + 16 * u.k;
                if (tx + tw + 4 * u.k >= nameR) return;
                ImVec2 ta(tx, my - 11 * u.k), tb(tx + tw, my + 11 * u.k);
                u.dl->AddRectFilled(ta, tb, bg, 11 * u.k);
                u.dl->AddRect(ta, tb, fg, 11 * u.k, 0, 1.0f);
                u.TextCentered(ta, tb, s, 0.95f, fg);
                tx += tw + 8 * u.k;
            };
            if (lead) tag("LEADER", kGreen, kGreenBg);
            if (m.nosong) tag("NO SONG", kRed, kRedBg);
        }
        if (assignable && u.FActivated(fiRow)) {
            sound::PlaySelectSE();
            CmdPickMode("assigned", m.cid);
        }
        /* kick: owner only, never yourself; pad needs a 2-step confirm */
        if (owner && !you) {
            float ks = 38 * u.k;
            ImVec2 ka(rb.x - 8 * u.k - ks, (ra.y + rb.y) * 0.5f - ks * 0.5f), kb(ka.x + ks, ka.y + ks);
            int fiKick = u.Focus(ImVec2(ka.x - 6 * u.k, ka.y - 6 * u.k),
                                 ImVec2(kb.x + 6 * u.k, kb.y + 6 * u.k), true, true);
            bool kh = u.FHover(fiKick);
            u.dl->AddRectFilled(ka, kb, (kh || armed) ? kRed : kRedBg, 5 * u.k);
            if (kh && s_padMode)
                u.Ring(ImVec2(ka.x - 4 * u.k, ka.y - 4 * u.k),
                       ImVec2(kb.x + 4 * u.k, kb.y + 4 * u.k), 5 * u.k);
            u.dl->AddRect(ka, kb, kRed, 5 * u.k, 0, (kh || armed) ? 2.0f : 1.2f);
            float h = ks * 0.26f; ImVec2 c((ka.x + kb.x) * 0.5f, (ka.y + kb.y) * 0.5f);
            ImU32 xc = (kh || armed) ? kInk : kRed;
            u.dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), xc, 2.5f * u.k);
            u.dl->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), xc, 2.5f * u.k);
            if (armed) {
                const char* hint = "A again to remove";
                ImVec2 hs = u.Size(hint, 1.15f);
                float ph = hs.y + 12 * u.k, pw = hs.x + 20 * u.k;
                float my = (ka.y + kb.y) * 0.5f;
                ImVec2 pa(ka.x - 8 * u.k - pw, my - ph * 0.5f), pb(pa.x + pw, pa.y + ph);
                u.dl->AddRectFilled(pa, pb, kRedBg, ph * 0.5f);
                u.dl->AddRect(pa, pb, kRed, ph * 0.5f, 0, 1.2f);
                u.TextCentered(pa, pb, hint, 1.15f, kRed);
            }
            if (u.FPadActivated(fiKick)) {
                if (armed) {
                    sound::PlayCancelSE();
                    CmdKick(m.cid);
                    s_armCid[0] = 0;
                } else {
                    sound::PlaySelectSE();
                    strncpy(s_armCid, m.cid, sizeof(s_armCid) - 1);
                    s_armCid[sizeof(s_armCid) - 1] = 0;
                    s_armUntil = NavNowMs() + 3000;
                }
            } else if (u.FActivated(fiKick)) {
                sound::PlayCancelSE();      /* touch stays single-tap */
                CmdKick(m.cid);
            }
        }
    }

    /* ---- footer: status ---- */
    char foot[200];
    const char* leadName = "";
    for (int i = 0; i < s.memberCount; i++)
        if (strcmp(s.members[i].cid, s.leaderCid) == 0) leadName = s.members[i].name;
    ImU32 footCol = kMuted;
    char noSong[120] = ""; int noSongN = 0;
    for (int i = 0, off = 0; i < s.memberCount && off < (int)sizeof(noSong) - 44; i++)
        if (s.members[i].nosong)
            off += snprintf(noSong + off, sizeof(noSong) - off, "%s%s", noSongN++ ? ", " : "",
                            strcmp(s.members[i].cid, s.myCid) == 0 ? "You" : s.members[i].name);
    if (!s.connected) {
        int left = s.offlineMs >= 0 ? 30 - s.offlineMs / 1000 : 30;
        snprintf(foot, sizeof(foot), "Connection lost - reconnecting (leaving the room in %ds)",
                 left < 0 ? 0 : left);
        footCol = kYellow;
    } else if (s.voteKind[0])
        snprintf(foot, sizeof(foot), "%s vote: %d/%d agreed (%ds left)",
                 strcmp(s.voteKind, "pause") == 0 ? "Pause" : "Resume",
                 s.voteAgreed, s.voteNeed, s.voteEndsIn);
    else if (s.groupPaused)
        snprintf(foot, sizeof(foot), "GROUP PAUSED");
    else if (noSongN > 0) {
        snprintf(foot, sizeof(foot), "%s %s have this song", noSong,
                 noSongN == 1 ? "doesn't" : "don't");
        footCol = kRed;
    } else if (s.notReady[0])
        snprintf(foot, sizeof(foot), "Not ready: %s", s.notReady);
    else if (s.statusLine[0] && s.phase != 1 /* SELECT: leader can change, build it live */)
        snprintf(foot, sizeof(foot), "%s", s.statusLine);
    else {
        const char* mode = strcmp(s.pickMode, "owner") == 0 ? "Owner picks"
                         : (strcmp(s.pickMode, "assigned") == 0 ? "Assigned" : "Rotate");
        const char* sd = s.sameDiff ? (s.noFail > 0 ? "  |  Same difficulty  |  No Fail" : "  |  Same difficulty")
                                    : (s.noFail > 0 ? "  |  No Fail" : "");
        if (strcmp(s.leaderCid, s.myCid) == 0)
            snprintf(foot, sizeof(foot), "You're picking the song  |  %s%s", mode, sd);
        else
            snprintf(foot, sizeof(foot), "%s is picking the song  |  %s%s",
                     leadName[0] ? leadName : "The leader", mode, sd);
    }
    /* my status, right side of the footer */
    float reserve = 0.0f;
    for (int i = 0; i < s.memberCount; i++)
        if (strcmp(s.members[i].cid, s.myCid) == 0) {
            const char* ms = MemberStatus(s.members[i]);
            ImVec2 sz = u.Size(ms, 1.3f);
            u.Text(ImVec2(b.x - 30 * u.k - sz.x, b.y - 44 * u.k + (44 * u.k - sz.y) * 0.5f), ms, 1.3f,
                   MemberDot(s.members[i]));
            reserve = sz.x + 24 * u.k;
        }
    const GuideEnt g[] = {{"A", "OK"}, {"B", "Close"}};
    float gw = GuideW(u, g, 2);
    DrawGuide(u, ImVec2(b.x - 30 * u.k - gw - reserve, a.y),
              ImVec2(b.x - reserve, b.y), g, 2);
    Footer(u, a, b, foot, reserve + gw + 10 * u.k, footCol);
    if (assignMode)
        u.TextLeft(ImVec2(lx, cy1 - 30 * u.k), ImVec2(b.x - 30 * u.k, cy1), "Tap a member to make them the song picker", 1.15f, kMuted);
}

/* ----------------------------------------------------------- OPTIONS */
/* one radio card of an OPTIONS row; true when picked (not already set) */
static bool OptCard(const Ui& u, ImVec2 oa, ImVec2 ob, bool sel,
                    const char* label, const char* desc) {
    const float r = 12 * u.k;
    int fi = u.Focus(oa, ob);
    bool hov = u.FHover(fi);
    u.dl->AddRectFilled(oa, ob, sel ? kAccentBg : (hov ? kCardHov : kCard), r);
    u.dl->AddRect(oa, ob, (sel || hov) ? kAccent : kLine, r, 0, (sel || hov) ? 2.5f * u.k : 1.2f);
    if (hov && s_padMode) u.Ring(oa, ob, r);
    ImVec2 rc(oa.x + 24 * u.k, oa.y + 26 * u.k);
    u.dl->AddCircle(rc, 11 * u.k, sel ? kAccent : kMuted, 24, 2.0f * u.k);
    if (sel) u.dl->AddCircleFilled(rc, 6 * u.k, kAccent, 24);
    ImVec2 ls = u.Size(label, 1.6f);
    u.Text(ImVec2(oa.x + 44 * u.k, rc.y - ls.y * 0.5f), label, 1.6f, kText);
    char d[96];
    u.Fit(desc, ob.x - oa.x - 28 * u.k, 1.15f, d, sizeof(d));
    u.Text(ImVec2(oa.x + 14 * u.k, oa.y + 50 * u.k), d, 1.15f, kMuted);
    return u.FActivated(fi) && !sel;
}

/* ---------------------------------------------------------- ROOM CONFIG */
static void DrawRoomConfig(const Ui& u, const Snapshot& s, ImVec2 a, ImVec2 b) {
    float top = Header(u, a, b, "ROOM CONFIG", false);
    float bh = 48 * u.k, bw = 150 * u.k;
    ImVec2 ka(b.x - 30 * u.k - bw, a.y + (66 * u.k - bh) * 0.5f), kb(ka.x + bw, ka.y + bh);
    if (u.Button(ka, kb, "Done", Ui::B_PRIMARY, 1.4f)) {   /* focus 0 */
        sound::PlayCancelSE();
        s_screen = SC_ROOM;
    }
    /* same grid as OPTIONS: a title + hint, then a row of radio cards */
    const float x0 = a.x + 30 * u.k;
    const float ow = (b.x - a.x - 60 * u.k - 2 * 18 * u.k) / 3.0f, oh = 80 * u.k;
    float y = top + 14 * u.k;
    auto section = [&](const char* title, const char* sub) {
        u.Text(ImVec2(x0, y), title, 1.5f, kText);
        y += 30 * u.k;
        u.Text(ImVec2(x0, y), sub, 1.1f, kMuted);
        y += 26 * u.k;
    };
    auto col = [&](int i, ImVec2* oa, ImVec2* ob) {
        *oa = ImVec2(x0 + i * (ow + 18 * u.k), y);
        *ob = ImVec2(oa->x + ow, oa->y + oh);
    };
    ImVec2 oa, ob;

    /* who picks the song (focus 1..3) */
    section("Who picks the song", "Applies from the next round if a song is already running");
    struct Opt { const char* mode; const char* label; const char* desc; } opts[3] = {
        { "rotate",   "ROTATE",   "Everyone takes turns, one song each" },
        { "owner",    "OWNER",    "The room owner always picks" },
        { "assigned", "ASSIGNED", "One chosen member picks every song" } };
    for (int i = 0; i < 3; i++) {
        col(i, &oa, &ob);
        if (OptCard(u, oa, ob, strcmp(s.pickMode, opts[i].mode) == 0, opts[i].label, opts[i].desc)) {
            sound::PlaySelectSE();
            CmdPickMode(opts[i].mode, i == 2 ? s.myCid : nullptr);
        }
    }
    y += oh + 14 * u.k;

    /* difficulty (focus 4..5) and No Fail Mode (focus 6..7) share a row:
     * four narrower cards, each pair under its own title */
    const float qw = (b.x - a.x - 60 * u.k - 3 * 18 * u.k) / 4.0f;
    const bool nfRow = s.noFail >= 0;      /* the server has the option */
    const float x2 = x0 + 2 * (qw + 18 * u.k);
    u.Text(ImVec2(x0, y), "Difficulty", 1.5f, kText);
    if (nfRow) u.Text(ImVec2(x2, y), "No Fail Mode", 1.5f, kText);
    y += 30 * u.k;
    {
        char h[96];
        u.Fit("SAME: the closest chart if a game lacks that one", 2 * qw, 1.1f, h, sizeof(h));
        u.Text(ImVec2(x0, y), h, 1.1f, kMuted);
        if (nfRow) {
            u.Fit("The same for every player, whatever their own setting", 2 * qw, 1.1f, h, sizeof(h));
            u.Text(ImVec2(x2, y), h, 1.1f, kMuted);
        }
    }
    y += 26 * u.k;
    auto qcol = [&](int i, ImVec2* qa, ImVec2* qb) {
        *qa = ImVec2(x0 + i * (qw + 18 * u.k), y);
        *qb = ImVec2(qa->x + qw, qa->y + oh);
    };
    struct DOpt { const char* label; const char* desc; } dopts[2] = {
        { "FREE", "Everyone picks their own" },
        { "SAME", "Everyone plays the picker's" } };
    for (int i = 0; i < 2; i++) {
        qcol(i, &oa, &ob);
        if (OptCard(u, oa, ob, s.sameDiff == (i == 1), dopts[i].label, dopts[i].desc)) {
            sound::PlaySelectSE();
            CmdSameDiff(i == 1);
        }
    }
    if (nfRow) {
        DOpt nopts[2] = {
            { "OFF", "A song ends at zero life" },
            { "ON",  "Songs always play to the end" } };
        for (int i = 0; i < 2; i++) {
            qcol(2 + i, &oa, &ob);
            if (OptCard(u, oa, ob, (s.noFail > 0) == (i == 1), nopts[i].label, nopts[i].desc)) {
                sound::PlaySelectSE();
                CmdNoFail(i == 1);
            }
        }
    }
    y += oh + 14 * u.k;

    /* the fixed picker (ASSIGNED only): three columns of members */
    bool assigned = strcmp(s.pickMode, "assigned") == 0;
    if (assigned) {
        u.Text(ImVec2(x0, y), "Assigned picker", 1.5f, kText);
        y += 32 * u.k;
        float rowH = 44 * u.k;
        for (int i = 0; i < s.memberCount; i++) {
            const MemberInfo& m = s.members[i];
            int c3 = i % 3, row = i / 3;
            ImVec2 ra(x0 + c3 * (ow + 18 * u.k), y + row * rowH), rb(ra.x + ow, ra.y + rowH - 6 * u.k);
            if (rb.y > b.y - 50 * u.k) break;
            bool isPicker = strcmp(m.cid, s.leaderCid) == 0;
            int fi = u.Focus(ra, rb);
            bool hov = u.FHover(fi);
            u.dl->AddRectFilled(ra, rb, isPicker ? kAccentBg : (hov ? kCardHov : kCard), 8 * u.k);
            u.dl->AddRect(ra, rb, (isPicker || hov) ? kAccent : kLine, 8 * u.k, 0, 1.2f);
            if (hov && s_padMode) u.Ring(ra, rb, 8 * u.k);
            u.dl->AddCircleFilled(ImVec2(ra.x + 18 * u.k, (ra.y + rb.y) * 0.5f), 6 * u.k, MemberDot(m), 16);
            char line[64];
            snprintf(line, sizeof(line), "%s%s", m.name, strcmp(m.cid, s.myCid) == 0 ? " (You)" : "");
            u.TextLeft(ImVec2(ra.x + 34 * u.k, ra.y), ImVec2(rb.x - (isPicker ? 84 : 12) * u.k, rb.y), line, 1.35f,
                       m.connected ? kText : kDim);
            if (isPicker) {
                ImVec2 ps = u.Size("PICKER", 1.05f);
                u.Text(ImVec2(rb.x - 14 * u.k - ps.x, (ra.y + rb.y - ps.y) * 0.5f), "PICKER", 1.05f, kAccent);
            }
            if (u.FActivated(fi) && !isPicker) {
                sound::PlaySelectSE();
                CmdPickMode("assigned", m.cid);
            }
        }
    }
    /* the room's web key, left of Done (registered last: the focus
     * numbers above stay what they are) */
    {
        ImVec2 wa(ka.x - 14 * u.k - 170 * u.k, ka.y), wb(ka.x - 14 * u.k, kb.y);
        if (u.Button(wa, wb, "Web Key", Ui::B_NORMAL, 1.4f)) {
            sound::PlayEnterSE();
            CmdWebKey();
            s_screen = SC_WEB_KEY;
        }
    }
    const GuideEnt g[] = {{"A", "OK"}, {"B", "Back"}};
    float gw = GuideW(u, g, 2);
    DrawGuide(u, ImVec2(b.x - 30 * u.k - gw, a.y), b, g, 2);
    Footer(u, a, b, assigned ? "Tap a member to make them the song picker"
                             : "These settings apply to everyone in the room",
           gw + 10 * u.k);
}

/* ------------------------------------------------------------- WEB KEY */
/* owner only: the key that opens this room's host controls on the web
 * dashboard (kick, song picker, room options) */
static void DrawWebKey(const Ui& u, const Snapshot& s, ImVec2 a, ImVec2 b) {
    float top = Header(u, a, b, "WEB DASHBOARD", false);
    float bh = 48 * u.k, bw = 150 * u.k;
    ImVec2 ka(b.x - 30 * u.k - bw, a.y + (66 * u.k - bh) * 0.5f), kb(ka.x + bw, ka.y + bh);
    if (u.Button(ka, kb, "Done", Ui::B_PRIMARY, 1.4f)) {
        sound::PlayCancelSE();
        s_screen = SC_ROOM_CONFIG;
    }
    const float x0 = a.x + 30 * u.k, x1 = b.x - 30 * u.k, mid = (x0 + x1) * 0.5f;
    float y = top + 40 * u.k;
    u.TextCentered(ImVec2(x0, y), ImVec2(x1, y + 34 * u.k),
                   "Manage this room from a web browser", 1.7f, kText);
    y += 50 * u.k;
    char line[128], fit[128];
    if (s.webUrl[0]) snprintf(line, sizeof(line), "Open  %s  and press Login", s.webUrl);
    else snprintf(line, sizeof(line), "Open the server's web page and press Login");
    u.Fit(line, x1 - x0, 1.4f, fit, sizeof(fit));
    u.TextCentered(ImVec2(x0, y), ImVec2(x1, y + 30 * u.k), fit, 1.4f, kMuted);
    y += 56 * u.k;

    ImVec2 fa(mid - 290 * u.k, y), fb(mid + 290 * u.k, y + 120 * u.k);
    u.dl->AddRectFilled(fa, fb, kField, 14 * u.k);
    u.dl->AddRect(fa, fb, s.webKey[0] ? kAccent : kLine, 14 * u.k, 0, 2.0f * u.k);
    if (s.webKey[0])
        u.TextCentered(fa, fb, s.webKey, 3.6f, kText);
    else
        u.TextCentered(fa, fb, s.connected ? "Asking the server..." : "Not connected",
                       1.7f, kDim);
    y = fb.y + 18 * u.k;
    u.TextCentered(ImVec2(x0, y), ImVec2(x1, y + 28 * u.k),
                   "Room key - type it into the Login box", 1.35f, kMuted);
    y += 40 * u.k;
    u.TextCentered(ImVec2(x0, y), ImVec2(x1, y + 26 * u.k),
                   "Whoever has it can kick players and change the room's settings.", 1.2f, kDim);
    y += 28 * u.k;
    u.TextCentered(ImVec2(x0, y), ImVec2(x1, y + 26 * u.k),
                   "It stops working when the room gets another owner.", 1.2f, kDim);

    const GuideEnt g[] = {{"A", "OK"}, {"B", "Back"}};
    float gw = GuideW(u, g, 2);
    DrawGuide(u, ImVec2(b.x - 30 * u.k - gw, a.y), b, g, 2);
    Footer(u, a, b, "Only the room owner can see this screen", gw + 10 * u.k);
}

static void DrawOptions(const Ui& u, const Snapshot& s, ImVec2 a, ImVec2 b) {
    float top = Header(u, a, b, "OPTIONS", false);
    float bh = 48 * u.k, bw = 150 * u.k;
    ImVec2 ka(b.x - 30 * u.k - bw, a.y + (66 * u.k - bh) * 0.5f), kb(ka.x + bw, ka.y + bh);
    if (u.Button(ka, kb, "Done", Ui::B_PRIMARY, 1.4f)) {   /* focus 0 */
        sound::PlayCancelSE();
        s_screen = InLobby() ? SC_ROOM : SC_HOME;
    }
    const float x0 = a.x + 30 * u.k;
    const float ow = (b.x - a.x - 60 * u.k - 2 * 18 * u.k) / 3.0f, oh = 80 * u.k;
    float y = top + 14 * u.k;
    auto section = [&](const char* title, const char* sub) {
        u.Text(ImVec2(x0, y), title, 1.5f, kText);
        y += 30 * u.k;
        u.Text(ImVec2(x0, y), sub, 1.1f, kMuted);
        y += 26 * u.k;
    };
    auto col = [&](int i, ImVec2* oa, ImVec2* ob) {
        *oa = ImVec2(x0 + i * (ow + 18 * u.k), y);
        *ob = ImVec2(oa->x + ow, oa->y + oh);
    };
    ImVec2 oa, ob;

    /* Server: off / LAN search / the online server (focus 1..3, pencil 4) */
    {
        mp::MpStatus ns;
        mp::GetStatus(&ns);
        const int mode = (int)ns.mode;
        /* live status, right-aligned on the title line */
        const char* l1 = mode == mp::kServerOff ? "Off - not connecting"
                       : s.connected ? "Connected"
                       : mode == mp::kServerCloud
                           ? (ns.state == mp::MpState::Error ? "Retrying..." : "Connecting (up to 60s)...")
                           : "Searching this network...";
        char line[160];
        if (mode == mp::kServerOff)
            snprintf(line, sizeof(line), "%s", l1);
        else
            snprintf(line, sizeof(line), "%s  %s", l1,
                     (!s.connected && ns.state == mp::MpState::Error && ns.last_error[0])
                         ? ns.last_error : ns.server);
        char fitted[96];
        u.Fit(line, (b.x - a.x) * 0.55f, 1.15f, fitted, sizeof(fitted));
        ImVec2 ls = u.Size(fitted, 1.15f);
        u.Text(ImVec2(b.x - 30 * u.k - ls.x, y + 6 * u.k), fitted, 1.15f,
               mode == mp::kServerOff ? kMuted : s.connected ? kGreen : kYellow);
        section("Server", "Where this console finds other players");
        char online[112];
        snprintf(online, sizeof(online), "%s", mp::CloudUrl());
        static const char* kLabel[3] = {"OFF", "LOCAL", "ONLINE"};
        const char* desc[3] = {"Multiplayer disabled, no network",
                               "PC server on this network (LAN)", online};
        for (int i = 0; i < 3; i++) {
            col(i, &oa, &ob);
            /* ONLINE: a pencil button in the top-right corner edits the
             * address; a tap on it must not also pick the card */
            ImVec2 pa(ob.x - 48 * u.k, oa.y + 6 * u.k), pb(ob.x - 8 * u.k, oa.y + 46 * u.k);
            bool onPencil = i == 2 && !s_padMode && u.Hover(pa, pb);
            bool picked = OptCard(u, oa, ob, mode == i, kLabel[i], desc[i]);
            if (picked && !onPencil) {
                sound::PlaySelectSE();
                mp::SetServerMode((mp::ServerMode)i);
                SystemBanner(i == 0 ? "Multiplayer off"
                           : i == 1 ? "Local server - searching the network..."
                                    : "Online server - connecting...");
            }
            if (i == 2) {
                int fi = u.Focus(pa, pb);
                bool hov = u.FHover(fi);
                u.dl->AddRectFilled(pa, pb, hov ? (u.down ? kCardDown : kCardHov) : kField, 10 * u.k);
                if (hov) u.Ring(pa, pb, 10 * u.k);
                else u.dl->AddRect(pa, pb, kLine, 10 * u.k, 0, 1.2f);
                IconPencil(u, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), 28 * u.k);
                if (u.FActivated(fi)) {
                    sound::PlayEnterSE();
                    OpenEditServer();
                }
            }
        }
    }
    y += oh + 14 * u.k;

    /* Notifications (focus 5..7) */
    section("Notifications", "How other players' events show up during a song");
    struct Opt { const char* label; const char* desc; } nopts[3] = {
        { "AVATARS", "Portrait pop-ups at the screen edge" },
        { "TEXT",    "Short text banners" },
        { "OFF",     "No player notifications" } };
    for (int i = 0; i < 3; i++) {
        col(i, &oa, &ob);
        if (OptCard(u, oa, ob, NotifyStyle() == i, nopts[i].label, nopts[i].desc)) {
            sound::PlaySelectSE();
            SetNotifyStyle(i);
        }
    }
    y += oh + 14 * u.k;

    /* Custom portraits (focus 8..9) */
    section("Custom portraits", "Portraits of modules this console doesn't have");
    Opt popts[2] = {
        { "OFF", "Show that character's default portrait" },
        { "ON",  "Download the image the other console sends" } };
    for (int i = 0; i < 2; i++) {
        col(i, &oa, &ob);
        if (OptCard(u, oa, ob, Config::mpRemoteAvatars == (i == 1), popts[i].label,
                    popts[i].desc)) {
            sound::PlaySelectSE();
            SetRemoteAvatars(i == 1);
        }
    }
    const GuideEnt g[] = {{"A", "OK"}, {"B", "Back"}};
    float gw2 = GuideW(u, g, 2);
    DrawGuide(u, ImVec2(b.x - 30 * u.k - gw2, a.y), b, g, 2);
    Footer(u, a, b, "Saved on this console", gw2 + 10 * u.k);
}

/* ------------------------- pad nav driver ------------------------- */
static int AxisStep(int axis, int dir, uint64_t now) {
    if (dir == 0) { s_lastDir[axis] = 0; return 0; }
    if (dir != s_lastDir[axis]) {
        s_lastDir[axis] = dir; s_holdStart[axis] = now;
        s_lastStep[axis] = now; return dir;
    }
    if (now - s_holdStart[axis] >= 400 && now - s_lastStep[axis] >= 120) {
        s_lastStep[axis] = now; return dir;
    }
    return 0;
}

/* dpad held or stick (dominant axis, ~0.5 deadzone) -> -1/0/+1 steps */
static void NavDirs(uint64_t held, int sx, int sy, uint64_t now,
                    int& dx, int& dy) {
    int hx = (held & nn::hid::Right ? 1 : 0) - (held & nn::hid::Left ? 1 : 0);
    int hy = (held & nn::hid::Down ? 1 : 0) - (held & nn::hid::Up ? 1 : 0);
    if (hx == 0 && hy == 0 &&
        (sx > 16384 || sx < -16384 || sy > 16384 || sy < -16384)) {
        if (std::abs(sx) > std::abs(sy)) hx = sx > 0 ? 1 : -1;
        else hy = sy > 0 ? 1 : -1;
    }
    dx = AxisStep(0, hx, now);
    dy = AxisStep(1, hy, now);
}

/* spatial move: nearest item whose centre lies in that direction,
 * off-axis distance weighted x2; no wrap */
static void MoveFocus(int dx, int dy) {
    if (s_itemCount <= 0) return;
    if (s_focusIdx < 0 || s_focusIdx >= s_itemCount ||
        !s_items[s_focusIdx].enabled) {
        /* stale focus (item removed/changed): keep the nearest index */
        int best = -1, bd = 1 << 30;
        for (int i = 0; i < s_itemCount; i++)
            if (s_items[i].enabled) {
                int d = i - s_focusIdx; if (d < 0) d = -d;
                if (d < bd) { bd = d; best = i; }
            }
        s_focusIdx = best;
        return;
    }
    const FocusItem& cur = s_items[s_focusIdx];
    float cx = (cur.a.x + cur.b.x) * 0.5f, cy = (cur.a.y + cur.b.y) * 0.5f;
    int best = -1; float bestScore = 1e30f;
    for (int i = 0; i < s_itemCount; i++) {
        if (i == s_focusIdx || !s_items[i].enabled) continue;
        float ix = (s_items[i].a.x + s_items[i].b.x) * 0.5f;
        float iy = (s_items[i].a.y + s_items[i].b.y) * 0.5f;
        float ddx = ix - cx, ddy = iy - cy, on, off;
        if (dx > 0)      { if (ddx < 4.0f) continue; on = ddx;  off = fabsf(ddy); }
        else if (dx < 0) { if (ddx > -4.0f) continue; on = -ddx; off = fabsf(ddy); }
        else if (dy > 0) { if (ddy < 4.0f) continue; on = ddy;  off = fabsf(ddx); }
        else             { if (ddy > -4.0f) continue; on = -ddy; off = fabsf(ddx); }
        float score = on + off * 2.0f;
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best >= 0) {
        s_focusIdx = best;
        s_armCid[0] = 0;                  /* moving focus disarms kick/exit */
        s_exitArmed = false;
        sound::PlaySelectSE();
    }
}

/* ---------------------------------------------------------------- main */
void DrawPanel() {
    bool open = OverlayOpen();
    if (open && !s_prevOpen) {
        /* (re)opening: land on the right screen */
        s_screen = InLobby() ? SC_ROOM : SC_HOME;
        s_kbArmed = false;
        /* drop edges latched before opening (e.g. the ZR that opened us) */
        input::NavClear();
        /* opened by a pad (explicit request, or ZR/stick still held) */
        int osx = 0, osy = 0;
        input::NavStick(&osx, &osy);
        s_padMode = s_padModeReq ||
                    (input::NavHeld() & ~(uint64_t)nn::hid::Minus) != 0 ||
                    osx > 16384 || osx < -16384 || osy > 16384 || osy < -16384;
        s_padModeReq = false;
        s_armCid[0] = 0;
        s_zrIgnoreUntil = NavNowMs() + 300;
        s_lastScreen = (Screen)-1;   /* force the default focus on reopen */
    }
    /* OUR keyboard only closes when the overlay is closed OR when the
     * open overlay has left its text screen (a VirtualKeyboard opened
     * by Edit Name / Join Room must never outlive song select); another
     * owner's keyboard (SongQuickJump) is left alone. */
    bool ourKb = VirtualKeyboard::IsOpenFor(s_nameBuf)
              || VirtualKeyboard::IsOpenFor(s_codeBuf)
              || VirtualKeyboard::IsOpenFor(s_urlBuf);
    bool onTextScreen = s_screen == SC_EDIT_NAME || s_screen == SC_JOIN_ROOM ||
                        s_screen == SC_EDIT_SERVER;
    if (ourKb && (!open || !onTextScreen)) {
        VirtualKeyboard::Close();
        s_kbArmed = false;
        if (onTextScreen)
            s_screen = s_screen == SC_EDIT_SERVER ? SC_OPTIONS : InLobby() ? SC_ROOM : SC_HOME;
    }
    if (!open && s_prevOpen) {
        VirtualKeyboard::SetPadMode(false);
        s_armCid[0] = 0;
        s_joinKbBack = false;
    }
    s_prevOpen = open;
    if (!open) return;

    Snapshot s;
    GetSnapshot(&s);

    /* screen fix-ups driven by lobby state */
    bool inLobby = InLobby();
    if (inLobby && (s_screen == SC_HOME || s_screen == SC_JOIN_ROOM)) GoHomeOrRoom();
    if (!inLobby && (s_screen == SC_ROOM || s_screen == SC_ROOM_CONFIG ||
                     s_screen == SC_WEB_KEY)) GoHomeOrRoom();
    if ((s_screen == SC_ROOM_CONFIG || s_screen == SC_WEB_KEY) &&
        (strcmp(s.ownerCid, s.myCid) != 0 || !s.connected))
        s_screen = SC_ROOM;
    /* JOIN closed the keyboard, but its screen stays until we're in */
    if (s_joinKbBack) {
        s_joinKbBack = false;
        if (s_screen == SC_JOIN_ROOM && !VirtualKeyboard::IsOpen()) {
            VirtualKeyboard::Open(s_codeBuf, sizeof(s_codeBuf), "JOIN", OnCodeSubmit);
            s_kbArmed = true;
        }
    }
    /* keyboard dismissed (CLOSE / tap outside) while editing -> cancel */
    if ((s_screen == SC_EDIT_NAME || s_screen == SC_JOIN_ROOM ||
         s_screen == SC_EDIT_SERVER) && s_kbArmed && !VirtualKeyboard::IsOpen())
        GoHomeOrRoom();
    {   /* the server refused the code: the dead-end sound */
        static int s_prevJoin = JOIN_IDLE;
        if (s.joinState == JOIN_FAILED && s_prevJoin == JOIN_PENDING &&
            s_screen == SC_JOIN_ROOM)
            sound::PlayCancelSE();
        s_prevJoin = s.joinState;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    float k = ds.y / 720.0f;
    bool textScreen = s_screen == SC_EDIT_NAME || s_screen == SC_JOIN_ROOM ||
                      s_screen == SC_EDIT_SERVER;

    /* ---------------- pad navigation input ---------------- */
    s_padA = false;
    {
        uint64_t edges = input::NavConsume(~(uint64_t)nn::hid::Minus);
        uint64_t held  = input::NavHeld();
        int nsx = 0, nsy = 0;
        input::NavStick(&nsx, &nsy);
        uint64_t now = NavNowMs();
        if (edges || nsx > 16384 || nsx < -16384 || nsy > 16384 || nsy < -16384)
            s_padMode = true;
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ||
            ImGui::IsMouseClicked(0) || ImGui::IsMouseDown(0))
            s_padMode = false;

        /* screen change -> reset focus + disarm kick */
        if (s_screen != s_lastScreen) {
            bool fromOptions = s_lastScreen == SC_OPTIONS;
            Screen prev = s_lastScreen;
            s_lastScreen = s_screen;
            s_armCid[0] = 0;
            if (s_screen == SC_HOME)
                s_focusIdx = fromOptions ? 4          /* back on the Options button */
                           : (s.connected ? 2 : 1);   /* [X, EDIT, CREATE, JOIN, OPTIONS] */
            else if (s_screen == SC_OPTIONS)   /* [Done, off, local, online, pencil, n1..n3, p1..p2] */
                s_focusIdx = prev == SC_EDIT_SERVER ? 4 : 1 + (int)mp::GetServerMode();
            else if (s_screen == SC_ROOM)
                s_focusIdx = (strcmp(s.ownerCid, s.myCid) == 0) ? 2 : 1;
            else if (s_screen == SC_ROOM_CONFIG)
                s_focusIdx = strcmp(s.pickMode, "owner") == 0 ? 2
                           : strcmp(s.pickMode, "assigned") == 0 ? 3 : 1;
            else
                s_focusIdx = 0;
        }

        if (s_padMode) {
            if ((edges & nn::hid::ZR) && now >= s_zrIgnoreUntil) {
                /* ZR closes from any screen */
                sound::PlayCancelSE();
                SetOverlayOpen(false);
                s_armCid[0] = 0;
            } else if (textScreen) {
                int dx = 0, dy = 0;
                NavDirs(held, nsx, nsy, now, dx, dy);
                const char* buf = s_screen == SC_EDIT_NAME ? s_nameBuf
                                : s_screen == SC_EDIT_SERVER ? s_urlBuf : s_codeBuf;
                bool wasEmpty = !buf[0];
                VirtualKeyboard::PadInput(edges, dx, dy);
                if ((edges & nn::hid::B) && wasEmpty) {
                    sound::PlayCancelSE();
                    GoHomeOrRoom();
                }
            } else {
                if (edges & nn::hid::B) {
                    s_armCid[0] = 0;
                    sound::PlayCancelSE();
                    if (s_screen == SC_WEB_KEY) s_screen = SC_ROOM_CONFIG;
                    else if (s_screen == SC_ROOM_CONFIG) s_screen = SC_ROOM;
                    else if (s_screen == SC_OPTIONS)
                        s_screen = InLobby() ? SC_ROOM : SC_HOME;
                    else SetOverlayOpen(false);
                } else {
                    int dx = 0, dy = 0;
                    NavDirs(held, nsx, nsy, now, dx, dy);
                    if (dx) MoveFocus(dx, 0);
                    if (dy) MoveFocus(0, dy);
                    s_padA = (edges & nn::hid::A) != 0;
                }
            }
        }
        if (s_armCid[0] && NavNowMs() > s_armUntil) s_armCid[0] = 0;
        if (s_exitArmed && NavNowMs() > s_exitArmUntil) s_exitArmed = false;

        VirtualKeyboard::SetPadMode(s_padMode && textScreen);
        s_itemCount = 0;                     /* rebuilt by the draw below */
    }

    /* panel rect: 84% of the screen; text screens are a compact dialog
     * sitting above the keyboard */
    ImVec2 sz(ds.x * 0.84f, ds.y * 0.84f);
    ImVec2 a((ds.x - sz.x) * 0.5f, (ds.y - sz.y) * 0.5f);
    ImVec2 b(a.x + sz.x, a.y + sz.y);
    if (textScreen) b.y = a.y + 280.0f * k;

    ImGui::SetNextWindowPos(a, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(b.x - a.x, b.y - a.y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    if (!BeginWin("##mpOverlay", nullptr, flags)) { EndWin(); return; }
    ImGui::g_imguiHasFocus = true;

    Ui u;
    u.dl = ImGui::GetWindowDrawList();
    u.font = ImGui::GetFont();
    u.k = k;
    u.fs = u.font->FontSize;
    u.mouse = io.MousePos;
    u.clicked = ImGui::IsMouseClicked(0);
    u.down = ImGui::IsMouseDown(0);
    u.t = (float)ImGui::GetTime();

    /* dim the game, then the panel */
    u.dl->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(0, 0, 0, 120));
    u.dl->AddRectFilled(ImVec2(a.x + 4 * k, a.y + 6 * k), ImVec2(b.x + 4 * k, b.y + 6 * k), IM_COL32(0, 0, 0, 90), 18 * k);
    u.dl->AddRectFilled(a, b, kBg, 18 * k);
    u.dl->AddRect(a, b, kLine, 18 * k, 0, 1.5f);

    switch (s_screen) {
        case SC_HOME:        DrawHome(u, s, a, b); break;
        case SC_EDIT_NAME:   DrawTextScreen(u, a, b, "EDIT NAME", s_nameBuf, "SAVE", false,
                                            OnNameSubmit, "Type your player name"); break;
        case SC_JOIN_ROOM: {
            char note[80]; ImU32 noteCol; bool bad;
            JoinNote(s, note, sizeof(note), &noteCol, &bad);
            DrawTextScreen(u, a, b, "JOIN ROOM", s_codeBuf, "JOIN", true,
                           OnCodeSubmit, "Enter the 4-letter room code", false,
                           note, noteCol, bad);
            break;
        }
        case SC_ROOM:        DrawRoom(u, s, a, b); break;
        case SC_ROOM_CONFIG: DrawRoomConfig(u, s, a, b); break;
        case SC_WEB_KEY:     DrawWebKey(u, s, a, b); break;
        case SC_OPTIONS:     DrawOptions(u, s, a, b); break;
        case SC_EDIT_SERVER: DrawTextScreen(u, a, b, "ONLINE SERVER", s_urlBuf, "SAVE", false,
                                            OnUrlSubmit, "Empty = mikumod.evanthomas.dev", true); break;
    }

    EndWin();
    /* the keyboard is a window of its own: a tap on this panel raises the
     * panel, and its screen dim, over the keys - put them back on top */
    if (textScreen)
        if (ImGuiWindow* kb = ImGui::FindWindowByName("##VirtualKeyboardOverlay"))
            ImGui::BringWindowToDisplayFront(kb);
}

bool PadModeActive() { return OverlayOpen() && s_padMode; }
void RequestPadMode() { s_padModeReq = true; }

/* ================================================================
 * Banners (top-center, fading)
 * ============================================================== */
void DrawBanners() {
    Snapshot s;
    GetSnapshot(&s);
    if (s.bannerCount == 0) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float k = ds.y / 720.0f;
    /* in a song: small, top-left below the life bar, newest 2 only (keeps
     * the note area clear); otherwise top-centre (above the overlay panel
     * while it is open, else below the top UI) */
    const bool song = s.inSong && !OverlayOpen();
    float top = song ? 62 * k : (OverlayOpen() ? 6 * k : 60 * k);
    if (song && AutoPlay::IndicatorVisible())
        top += 30 * k;   /* banners stack below the AUTO pill */
    int i0 = song && s.bannerCount > 2 ? s.bannerCount - 2 : 0;
    for (int i = i0; i < s.bannerCount; i++) {
        float ttl = s.banners[i].ttl;
        float alpha = ttl > 2.5f ? (3.0f - ttl) * 2.0f
                    : (ttl < 0.8f ? ttl / 0.8f : 1.0f);
        const char* txt = s.banners[i].text;
        if (song)
            ImGui::SetNextWindowPos(ImVec2(14 * k, top + (i - i0) * 26 * k),
                                    ImGuiCond_Always, ImVec2(0.0f, 0.0f));
        else
            ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, top + i * 34 * k),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha((song ? 0.45f : 0.55f) * alpha);
        char id[16];
        snprintf(id, sizeof(id), "##mpb%d", i);
        if (!BeginWin(id, nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_AlwaysAutoResize)) {
            EndWin();
            continue;
        }
        ImGui::SetWindowFontScale(song ? 1.0f : 1.35f);
        ImGui::TextColored(ImVec4(1, 1, 1, alpha), "%s", txt);
        EndWin();
    }
}

/* ================================================================
 * Leaderboard (in-song) + waiting + standings
 * ============================================================== */
/* per-member race colour (stable for the room): palette[index of that
 * cid in Snapshot.members]. The game's own fill is cyan / yellow / red by
 * life state and the clear zone is green, so those hues come last. */
static const ImU32 kRacePalette[8] = {
    IM_COL32(165, 110, 255, 255), IM_COL32(255, 150, 40, 255),
    IM_COL32(70, 140, 255, 255),  IM_COL32(255, 75, 145, 255),
    IM_COL32(170, 240, 60, 255),  IM_COL32(230, 70, 255, 255),
    IM_COL32(45, 225, 165, 255),  IM_COL32(255, 220, 70, 255),
};

static ImU32 RaceColor(const Snapshot& s, const char* cid) {
    for (int i = 0; i < s.memberCount && i < 8; i++)
        if (strcmp(s.members[i].cid, cid) == 0)
            return kRacePalette[i];
    return IM_COL32(180, 180, 180, 255);
}

/* smoothed displayed pct per other-player cid; reset out of song */
static struct { char cid[20]; float d; } s_race[8];
static int s_raceN = 0;

static ImU32 _shade(ImU32 c, float f, int a) {
    int r = (int)((c & 0xff) * f), g = (int)(((c >> 8) & 0xff) * f),
        b = (int)(((c >> 16) & 0xff) * f);
    return IM_COL32(r > 255 ? 255 : r, g > 255 ? 255 : g,
                    b > 255 ? 255 : b, a);
}

static mp::avatar::Ref RaceAvatarRef(const Snapshot& s, const char* cid) {
    for (int i = 0; i < s.memberCount; i++)
        if (strcmp(s.members[i].cid, cid) == 0)
            return MemberRef(s, s.members[i]);
    return mp::avatar::Ref{};
}

/* mini pointy-top DIVA hexagon framed in the player's race colour */
static void DrawRaceAvatar(ImDrawList* dl, float k, ImVec2 c, float r,
                           const mp::avatar::Ref& av, ImU32 raceCol,
                           bool connected) {
    ImVec2 p[6];
    int a = connected ? 255 : 130;
    HexFill(dl, ImVec2(c.x, c.y + 2.0f * k), r + 1.0f * k,
            IM_COL32(0, 0, 0, connected ? 115 : 55),
            IM_COL32(0, 0, 0, connected ? 115 : 55));
    ImU32 f0 = connected ? _shade(raceCol, 1.22f, 255)
                         : IM_COL32(150, 150, 150, a);
    ImU32 f1 = connected ? _shade(raceCol, 0.68f, 255)
                         : IM_COL32(105, 105, 105, a);
    HexFill(dl, c, r, f0, f1);
    HexPts(c, r - 1.2f * k, p);
    ImVec2 bevel[3] = { p[4], p[5], p[0] };
    dl->AddPolyline(bevel, 3,
                    IM_COL32(255, 255, 255, connected ? 140 : 60),
                    0, 1.6f * k);
    float ri = r * 0.81f;
    HexFill(dl, c, ri + 1.4f * k,
            IM_COL32(255, 255, 255, connected ? 245 : 130),
            IM_COL32(255, 255, 255, connected ? 245 : 130));
    HexFill(dl, c, ri,
            connected ? IM_COL32(255, 232, 242, 255)
                      : IM_COL32(200, 200, 200, a),
            connected ? IM_COL32(255, 220, 198, 255)
                      : IM_COL32(170, 170, 170, a));
    void* tex = nullptr;
    float uv0[2], uv1[2];
    if (mp::avatar::Resolve(av, &tex, uv0, uv1) == 1) {
        ImU32 tint = connected ? IM_COL32_WHITE
                               : IM_COL32(145, 145, 145, a);
        HexFill(dl, c, ri, tint, tint, (ImTextureID)tex,
                ImVec2(uv0[0], uv0[1]), ImVec2(uv1[0], uv1[1]));
    } else {
        Ui u{};
        u.dl = dl;
        u.k = k;
        ImU32 sil = connected ? _shade(raceCol, 0.95f, 240)
                              : IM_COL32(140, 140, 140, a);
        IconPerson(u, ImVec2(c.x, c.y - ri * 0.08f), ri * 0.58f, sil, sil);
    }
}

void DrawGaugeRace() {
    Snapshot s;
    GetSnapshot(&s);
    if (!s.inSong || s.boardCount == 0 || OverlayOpen() ||
        s.resultsAgeMs >= 0) { s_raceN = 0; return; }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;
    const ImGuiIO& io = ImGui::GetIO();
    float k = io.DisplaySize.y / 720.0f;
    float dt = io.DeltaTime;
    auto X = [&](float p) {
        p = p < 0 ? 0 : (p > 100 ? 100 : p);
        return (130.0f + 11.0f * p) * k;
    };
    /* smooth each other player's pct toward its target */
    int n = 0;
    float ds_[8]; ImU32 cs_[8]; bool dc_[8]; mp::avatar::Ref av_[8];
    for (int i = 0; i < s.boardCount && n < 8; i++) {
        const auto& b = s.board[i];
        if (b.me) continue;
        int j;
        for (j = 0; j < s_raceN; j++)
            if (strcmp(s_race[j].cid, b.cid) == 0) break;
        if (j == s_raceN && s_raceN < 8) {
            strncpy(s_race[j].cid, b.cid, sizeof(s_race[j].cid) - 1);
            s_race[j].d = b.pct;
            s_raceN++;
        }
        if (j < s_raceN) {
            s_race[j].d += (b.pct - s_race[j].d) * (dt * 8.0f > 1 ? 1 : dt * 8.0f);
            ds_[n] = s_race[j].d;
            cs_[n] = RaceColor(s, b.cid);
            dc_[n] = !b.connected;
            av_[n] = RaceAvatarRef(s, b.cid);
            n++;
        }
    }
    /* sort by displayed pct, largest first */
    for (int i = 0; i + 1 < n; i++)
        for (int j = i + 1; j < n; j++)
            if (ds_[j] > ds_[i]) {
                float tf = ds_[i]; ds_[i] = ds_[j]; ds_[j] = tf;
                ImU32 tc = cs_[i]; cs_[i] = cs_[j]; cs_[j] = tc;
                bool tb = dc_[i]; dc_[i] = dc_[j]; dc_[j] = tb;
                mp::avatar::Ref ta = av_[i]; av_[i] = av_[j]; av_[j] = ta;
            }
    float myx = X(s.myPct);
    /* The game's BORDER / G / E ticks must stay on top of the other
     * players' bars, and ImGui can only draw over the game: so the bars
     * leave a slot open at each tick (stem + its dark outline, measured:
     * -1.4 .. +3.3 units around 130 + 11 * pct) and the game's own tick
     * shows through. */
    float gapA[3], gapB[3]; int ng = 0;
    for (int q = 0; q < 3; q++) {
        if (s.gaugeTick[q] < 0.0f || s.gaugeTick[q] > 100.0f) continue;
        const float tx0 = (130.0f + 11.0f * s.gaugeTick[q]) * k;
        gapA[ng] = tx0 - 1.4f * k; gapB[ng] = tx0 + 3.3f * k; ng++;
    }
    for (int i = 0; i + 1 < ng; i++)            /* left to right */
        for (int j = i + 1; j < ng; j++)
            if (gapA[j] < gapA[i]) {
                float t = gapA[i]; gapA[i] = gapA[j]; gapA[j] = t;
                t = gapB[i]; gapB[i] = gapB[j]; gapB[j] = t;
            }
    /* calls seg(a, b) for every part of [x0, x1] outside the tick slots */
    auto spans = [&](float x0, float x1, auto&& seg) {
        float x = x0;
        for (int q = 0; q < ng && x < x1; q++) {
            if (gapB[q] <= x) continue;
            if (gapA[q] >= x1) break;
            if (gapA[q] > x) seg(x, gapA[q]);
            x = gapB[q];
        }
        if (x < x1) seg(x, x1);
    };
    /* lead fill over [x(myPct), x(d)] for whoever is ahead; drawing
     * nearest-last makes each visible segment a lead over the next */
    bool anyLead = false;
    for (int i = 0; i < n; i++) {           /* farthest first */
        float d = ds_[i];
        if (d <= s.myPct + 0.05f) continue;
        anyLead = true;
        ImU32 c = cs_[i];
        int a = dc_[i] ? 80 : 235;
        const float y0 = 692 * k, y1 = 710 * k, xe = X(d);
        spans(myx, xe, [&](float xa, float xb) {
            dl->AddRectFilledMultiColor(
                ImVec2(xa, y0), ImVec2(xb, y1), _shade(c, 1.35f, a), _shade(c, 1.35f, a),
                _shade(c, 1.0f, a), _shade(c, 1.0f, a));
            dl->AddLine(ImVec2(xa, y0), ImVec2(xb, y0),
                        IM_COL32(255, 255, 255, dc_[i] ? 28 : 70), 1.0f);
        });
        spans(xe - 2 * k, xe, [&](float xa, float xb) {
            dl->AddRectFilled(ImVec2(xa, y0), ImVec2(xb, y1), _shade(c, 0.6f, a), 0);
        });
    }
    /* dark seam where the local player's own fill ends */
    if (anyLead)
        spans(myx, myx + 2 * k, [&](float xa, float xb) {
            dl->AddRectFilled(ImVec2(xa, 692 * k), ImVec2(xb, 710 * k),
                              IM_COL32(0, 0, 0, 170), 0);
        });
    /* spread avatar centres slightly when markers sit at the same % so
     * every portrait stays visible; ds_ is sorted largest-first (right-to-left) */
    float tx[8], ax[8];
    for (int i = 0; i < n; i++) tx[i] = ax[i] = X(ds_[i]);
    const float minSep = 25.0f * k, xMin = 130.0f * k, xMax = 1230.0f * k;
    for (int pass = 0; pass < 5; pass++) {
        for (int i = n - 2; i >= 0; i--)
            if (ax[i] < ax[i + 1] + minSep) {
                float mid = 0.5f * (ax[i] + ax[i + 1]);
                ax[i]     = mid + 0.5f * minSep;
                ax[i + 1] = mid - 0.5f * minSep;
            }
        if (n > 0 && ax[n - 1] < xMin) ax[n - 1] = xMin;
        for (int i = n - 2; i >= 0; i--)
            if (ax[i] < ax[i + 1] + minSep) ax[i] = ax[i + 1] + minSep;
        if (n > 0 && ax[0] > xMax) ax[0] = xMax;
        for (int i = 1; i < n; i++)
            if (ax[i] > ax[i - 1] - minSep) ax[i] = ax[i - 1] - minSep;
    }
    /* pointers + avatars for EVERY other player (trailing first, leader on top) */
    const float hr = 13.5f * k, hcy = 667.5f * k;
    for (int i = n - 1; i >= 0; i--) {
        ImU32 c = cs_[i];
        int a = dc_[i] ? 110 : 255;
        ImU32 col = (c & 0x00ffffff) | ((ImU32)a << 24);
        ImVec2 tip(tx[i], 690.5f * k);
        if (fabsf(ax[i] - tx[i]) > 1.5f * k) {
            dl->AddLine(ImVec2(ax[i], hcy + hr - 1.0f * k),
                        ImVec2(tx[i], 683.0f * k),
                        IM_COL32(0, 0, 0, dc_[i] ? 70 : 170), 3.2f * k);
            dl->AddLine(ImVec2(ax[i], hcy + hr - 1.0f * k),
                        ImVec2(tx[i], 683.0f * k), col, 1.8f * k);
        }
        dl->AddTriangleFilled(
            ImVec2(tip.x - 5.5f * k, 682.0f * k),
            ImVec2(tip.x + 5.5f * k, 682.0f * k),
            tip, col);
        dl->AddTriangle(
            ImVec2(tip.x - 5.5f * k, 682.0f * k),
            ImVec2(tip.x + 5.5f * k, 682.0f * k),
            tip, IM_COL32(0, 0, 0, dc_[i] ? 64 : 170), 1.2f * k);
        DrawRaceAvatar(dl, k, ImVec2(ax[i], hcy), hr, av_[i], c, !dc_[i]);
    }
}

/* rank-change edge glow: 400 ms hold + 1500 ms cooldown, 1200 ms fade */
static int      s_glowBaseline = -1;
static int      s_glowCandidate = 0;
static uint64_t s_glowCandAt = 0;
static uint64_t s_glowLastAt = 0;
static uint64_t s_glowStart = 0;
static int      s_glowUp = 1;
static uint64_t _ms() {
    return (uint64_t)nn::os::GetSystemTick().GetInt64Value() * 1000
           / nn::os::GetSystemTickFrequency();
}

/* in-song placement badge state (Mario-Kart ordinal + DIVA styling) */
static int      s_placeDispRank = 0;
static int      s_placePrevRank = 0;
static int      s_placeCandRank = 0;
static uint64_t s_placeCandAt   = 0;
static uint64_t s_placeAnimAt   = 0;
static int      s_placeAnimUp   = 1;
static uint64_t s_placeEnterAt  = 0;
static float    s_placeAvoidA   = 1.0f;

void TriggerRankGlow(bool up) {
    s_glowUp = up ? 1 : 0;
    s_glowStart = _ms();
    s_glowLastAt = s_glowStart;
    s_placeAnimUp = up ? 1 : 0;
    s_placeAnimAt = s_glowStart;
    if (s_placeDispRank > 0) {
        s_placePrevRank = up ? (s_placeDispRank < 8 ? s_placeDispRank + 1 : 2)
                             : (s_placeDispRank > 1 ? s_placeDispRank - 1 : 1);
    }
}

void DrawRankGlow() {
    Snapshot s;
    GetSnapshot(&s);
    if (!s.inSong) { s_glowBaseline = -1; s_glowCandidate = 0;
                     s_glowStart = 0; return; }
    uint64_t now = _ms();
    int myRank = 0;
    for (int i = 0; i < s.boardCount; i++)
        if (s.board[i].me) { myRank = s.board[i].rank; break; }
    if (myRank <= 0) return;
    if (s.sinceStartMs < 8000 || s_glowBaseline < 0) {
        s_glowBaseline = myRank; s_glowCandidate = 0; }
    else if (myRank == s_glowBaseline) {
        s_glowCandidate = 0;               /* jitter back: drop the candidate */
    } else if (s_glowCandidate != myRank) {
        s_glowCandidate = myRank; s_glowCandAt = now;
    } else if (s_glowCandidate && myRank == s_glowCandidate &&
               now - s_glowCandAt >= 400 &&
               now - s_glowLastAt >= 1500) {
        s_glowUp = myRank < s_glowBaseline ? 1 : 0;
        s_glowStart = now; s_glowLastAt = now;
        s_glowBaseline = myRank; s_glowCandidate = 0;
    }
    if (!s_glowStart) return;
    float t = (float)(now - s_glowStart) / 1200.0f;
    if (t >= 1.0f) { s_glowStart = 0; return; }
    float alpha = 0.38f * (t < 0.12f ? t / 0.12f
                                    : 1.0f - (t - 0.12f) / 0.88f);
    if (alpha <= 0) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    ImU32 col = s_glowUp ? IM_COL32(70, 230, 130, 255)
                         : IM_COL32(175, 90, 255, 255);
    int a = (int)(alpha * 255);
    ImU32 edge = (col & 0x00ffffff) | ((ImU32)a << 24);
    float w = ds.x * 0.10f;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, ds.y),
                                edge, 0, 0, edge);
    dl->AddRectFilledMultiColor(ImVec2(ds.x - w, 0), ImVec2(ds.x, ds.y),
                                0, edge, edge, 0);
}

struct GameTextStyle {
    ImU32 top, mid, loTop, bot;   /* glossy fill stops */
    ImU32 dark;                   /* dark outer stroke colour */
    float darkK;                  /* dark stroke radius beyond bold, in k */
    ImU32 glow; float glowA;      /* faint outer glow (glowA 0 = none) */
};

/* game-style display text ("EXCELLENT" look): drop shadow, dark stroke,
 * white stroke, two-tone glossy fill, italic skew - all under `alpha`. */
static void DrawGameText(ImDrawList* dl, ImFont* f, float size,
                         ImVec2 center, const char* txt, float k,
                         const GameTextStyle& st,
                         float italic, float alpha) {
    ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, txt);
    ImVec2 p(center.x - ts.x / 2, center.y - ts.y / 2);
    float boldR = 2.2f * k, whiteR = boldR + 3.0f * k,
          darkR = boldR + st.darkK * k;
    int vtx0 = dl->VtxBuffer.Size;

    auto ring = [&](ImVec2 at, float r, int n, ImU32 col, int a) {
        ImU32 c = (col & 0x00ffffff) | ((ImU32)a << 24);
        for (int i = 0; i < n; i++) {
            float th = i * 6.2831853f / n;
            dl->AddText(f, size,
                        ImVec2(at.x + cosf(th) * r, at.y + sinf(th) * r),
                        c, txt);
        }
    };

    /* 1) drop shadow: the dark-stroked silhouette offset (5k, 6k) */
    ring(ImVec2(p.x + 5 * k, p.y + 6 * k), darkR, 24, st.dark,
         (int)(alpha * 0.45f * 255));
    /* 2) optional faint outer glow (GO! only) */
    if (st.glowA > 0)
        ring(p, darkR * 1.6f, 12, st.glow, (int)(st.glowA * alpha * 255));
    /* 3) dark stroke, then 4) white stroke inside it */
    ring(p, darkR, 24, st.dark, (int)(alpha * 255));
    ring(p, whiteR, 24, IM_COL32(255, 255, 255, 255), (int)(alpha * 255));
    /* 5) glossy fill: two passes clipped at the 52% split so the gloss
     * line runs through the middle */
    float splitY = p.y + ts.y * 0.52f;
    for (int half = 0; half < 2; half++) {
        ImVec2 c0(p.x - whiteR - 2, half ? splitY : p.y - whiteR - 2);
        ImVec2 c1(p.x + ts.x + whiteR + 2,
                  half ? p.y + ts.y + whiteR + 2 : splitY);
        dl->PushClipRect(c0, c1, true);
        int vf0 = dl->VtxBuffer.Size;
        ring(p, boldR, 8, IM_COL32(255, 255, 255, 255), 255);   /* embolden */
        dl->AddText(f, size, p, IM_COL32(255, 255, 255, 255), txt);
        int vf1 = dl->VtxBuffer.Size;
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(
            dl, vf0, vf1,
            ImVec2(p.x, half ? splitY : p.y),
            ImVec2(p.x, half ? p.y + ts.y : splitY),
            half ? st.loTop : st.top, half ? st.bot : st.mid);
        dl->PopClipRect();
    }
    /* italic skew + global alpha over everything added above */
    int aMul = (int)(alpha * 255);
    for (int i = vtx0; i < dl->VtxBuffer.Size; i++) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.pos.x += (center.y - v.pos.y) * italic;
        ImU32 a = (v.col >> 24) & 0xff;
        v.col = (v.col & 0x00ffffff) | ((ImU32)((a * aMul) / 255) << 24);
    }
}

void DrawCountdown() {
    Snapshot s;
    GetSnapshot(&s);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    const ImGuiIO& io = ImGui::GetIO();
    float k = io.DisplaySize.y / 720.0f;
    ImVec2 ctr(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f);
    ImFont* f = FontLoader::Countdown()
              ? FontLoader::Countdown()
              : (FontLoader::Big() ? FontLoader::Big() : ImGui::GetFont());
    float bandH = 170 * k;
    ImVec2 b0(0, ctr.y - bandH * 0.5f), b1(io.DisplaySize.x,
                                         ctr.y + bandH * 0.5f);
    /* the band + dim are constant for the whole countdown: fade in over
     * its first 200 ms, hold, fade out together with "GO!" - only the
     * digits animate */
    if (s.countdownAgeMs < 0 && s.goMs < 0) return;   /* nothing showing */
    float bandA = s.countdownAgeMs >= 0
        ? (s.countdownAgeMs < 200 ? s.countdownAgeMs / 200.0f : 1.0f)
        : (s.goMs >= 450 ? 1.0f - (s.goMs - 450) / 350.0f : 1.0f);
    if (bandA > 0) {
        dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                          IM_COL32(0, 0, 0, (int)(90 * bandA)));
        dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, (int)(110 * bandA)));
        int line = s.goMs >= 0 && s.goMs < 150 ? 255 : (int)(255 * bandA);
        dl->AddLine(b0, ImVec2(b1.x, b0.y),
                    IM_COL32(57, 197, 187, line), 3 * k);
        dl->AddLine(ImVec2(b0.x, b1.y), b1,
                    IM_COL32(57, 197, 187, line), 3 * k);
    }
    if (s.countdownMs >= 0) {
        int n = (s.countdownMs + 999) / 1000;
        if (n < 1) n = 1;
        /* only the digit animates: pop-in 200 ms (scale 1.5 -> 1.0,
         * alpha 0 -> 1), then fade the last 150 ms */
        int frac = s.countdownMs % 1000;
        float pop = frac > 800 ? 1.0f - (float)(frac - 800) / 200.0f : 0.0f;
        float fade = frac < 150 ? (float)frac / 150.0f : 1.0f;
        float scale = 1.0f + 0.5f * pop;
        float a = (1.0f - pop) * fade;
        char buf[8]; snprintf(buf, sizeof(buf), "%d", n);
        GameTextStyle st = {
            IM_COL32(255, 255, 255, 255), IM_COL32(205, 240, 255, 255),
            IM_COL32(150, 215, 255, 255), IM_COL32(95, 175, 255, 255),
            IM_COL32(18, 22, 55, 255), 7.0f,
            IM_COL32(57, 197, 187, 255), 0.0f};
        DrawGameText(dl, f, f->FontSize * scale, ctr, buf, k, st,
                     0.18f, a);
    } else if (s.goMs >= 0) {
        float t = (float)s.goMs;          /* 0..800 ms */
        if (t > 800.0f) return;
        float scale = t < 150 ? 0.8f + 0.32f * (t / 150.0f)
                    : t < 450 ? 1.12f - 0.12f * ((t - 150) / 300.0f)
                              : 1.0f;
        float a = t < 450 ? 1.0f : 1.0f - (t - 450) / 350.0f;
        GameTextStyle st = {
            IM_COL32(215, 255, 250, 255), IM_COL32(120, 245, 235, 255),
            IM_COL32(40, 200, 205, 255), IM_COL32(15, 150, 185, 255),
            IM_COL32(18, 22, 55, 255), 8.0f,
            IM_COL32(255, 255, 255, 255), 0.25f};
        DrawGameText(dl, f, f->FontSize * 1.25f * scale, ctr, "GO!",
                     k, st, 0.18f, a);
    }
}

void DrawPopups() {
    int n; const Popup* pp = Popups(&n);
    if (n == 0) return;
    Snapshot s;
    GetSnapshot(&s);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    const ImGuiIO& io = ImGui::GetIO();
    float k = io.DisplaySize.y / 720.0f;
    uint64_t now = _ms();
    /* live popups only, newest first, at most 3 */
    const Popup* live[3]; float lage[3]; int nlive = 0;
    for (int i = n - 1; i >= 0 && nlive < 3; i--) {
        float age = (float)(now - pp[i].bornAt);
        if (age > 3000.0f) continue;
        live[nlive] = &pp[i]; lage[nlive] = age; nlive++;
    }
    static const struct { const char* k; const char* tag; ImU32 c; } T[] = {
        {"combo", "COMBO %d", IM_COL32(255, 201, 51, 255)},
        {"low_life", "DANGER", IM_COL32(255, 85, 85, 255)},
        {"drop_out", "DROP OUT", IM_COL32(241, 80, 175, 255)},
        {"joined", "JOINED", IM_COL32(70, 230, 130, 255)},
        {"left", "LEFT", IM_COL32(150, 150, 150, 255)},
        {"disconnected", "OFFLINE", IM_COL32(150, 150, 150, 255)},
        {"reconnected", "BACK", IM_COL32(70, 230, 130, 255)},
        {"leader", "PICKER", IM_COL32(57, 197, 187, 255)},
        {"owner", "OWNER", IM_COL32(57, 197, 187, 255)},
    };
    for (int shown = 0; shown < nlive; shown++) {
        const Popup& p = *live[shown];
        float age = lage[shown];
        float a = age < 200 ? age / 200.0f
                : age > 2600 ? (3000.0f - age) / 400.0f : 1.0f;
        float x = s.inSong ? 10 * k
                  : (io.DisplaySize.x - 300 * k) * 0.5f;
        float y = s.inSong ? (140 + shown * 58) * k
                  : (60 + shown * 58) * k;
        x -= (1.0f - a) * 60 * k;
        /* avatar by cid */
        mp::avatar::Ref mod;
        for (int m = 0; m < s.memberCount; m++)
            if (strcmp(s.members[m].cid, p.cid) == 0) { mod = MemberRef(s, s.members[m]); break; }
        Ui u; u.dl = dl; u.font = ImGui::GetFont(); u.k = k;
        u.fs = ImGui::GetFont()->FontSize;
        DrawAvatar(u, ImVec2(x + 26 * k, y + 26 * k), 24 * k, mod,
                   false, true, false);
        char tag[48]; const char* tagp = "";
        for (auto& t : T)
            if (strcmp(t.k, p.kind) == 0) {
                snprintf(tag, sizeof(tag), t.tag,
                         strcmp(p.kind, "combo") == 0 ? (int)p.value : 0);
                tagp = tag;
                ImU32 c = t.c;
                ImFont* f = ImGui::GetFont();
                float nsz = f->FontSize * 1.1f, tsz = f->FontSize * 1.25f;
                /* truncate the name to 220k with ".." */
                char nm[44]; strncpy(nm, p.name, sizeof(nm) - 1);
                nm[sizeof(nm) - 1] = 0;
                float maxw = 220 * k;
                int nl = (int)strlen(nm);
                while (nl > 0 &&
                       f->CalcTextSizeA(nsz, FLT_MAX, 0, nm).x > maxw) {
                    nm[--nl] = 0;
                    if (nl >= 2) { nm[nl - 1] = '.'; nm[nl - 2] = '.'; }
                }
                float nw = f->CalcTextSizeA(nsz, FLT_MAX, 0, nm).x;
                float tw = f->CalcTextSizeA(tsz, FLT_MAX, 0, tagp).x;
                float pw = (nw > tw ? nw : tw) + 20 * k;
                if (pw > 260 * k) pw = 260 * k;
                dl->AddRectFilled(ImVec2(x + 56 * k, y),
                                  ImVec2(x + 56 * k + pw, y + 50 * k),
                                  IM_COL32(0, 0, 0, (int)(a * 170)), 10 * k);
                dl->AddText(f, nsz, ImVec2(x + 66 * k, y + 4 * k),
                            IM_COL32(255, 255, 255, (int)(a * 255)), nm);
                dl->AddText(f, tsz, ImVec2(x + 66 * k, y + 26 * k),
                            c & 0x00ffffff | ((ImU32)(a * 255) << 24), tagp);
                break;
            }
    }
}

static const char* OrdinalSuffix(int r) {
    if (r % 100 >= 11 && r % 100 <= 13) return "th";
    switch (r % 10) {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
    }
}

static GameTextStyle PlacementStyle(int rank, ImU32* outAccent) {
    /* ImFont::RenderChar emits one quad per glyph (top vertices at t=0,
     * bottom vertices at t=1), so `top` -> `bot` is the full vertical
     * gradient across the glyph. Keep both stops saturated (like GlowText)
     * so glyphs never look washed out in white on the Switch display. */
    switch (rank) {
    case 1: /* Radiant Gold -> Deep Amber-Orange (EXCELLENT / #1 look) */
        if (outAccent) *outAccent = IM_COL32(255, 210, 45, 255);
        return {
            IM_COL32(255, 215, 35, 255), IM_COL32(255, 165, 20, 255),
            IM_COL32(255, 140, 15, 255), IM_COL32(245, 68, 12, 255),
            IM_COL32(110, 22, 10, 255),  5.6f,
            IM_COL32(255, 175, 35, 255), 0.30f
        };
    case 2: /* Vivid Ice-Blue Silver -> Deep Cobalt Steel (GREAT) */
        if (outAccent) *outAccent = IM_COL32(120, 205, 255, 255);
        return {
            IM_COL32(145, 222, 255, 255), IM_COL32(105, 185, 245, 255),
            IM_COL32(85, 160, 235, 255),  IM_COL32(45, 102, 205, 255),
            IM_COL32(14, 28, 68, 255),    5.4f,
            IM_COL32(95, 195, 255, 255),  0.24f
        };
    case 3: /* Vivid Copper-Gold -> Deep Crimson-Bronze (STANDARD) */
        if (outAccent) *outAccent = IM_COL32(245, 145, 75, 255);
        return {
            IM_COL32(255, 172, 72, 255), IM_COL32(235, 125, 52, 255),
            IM_COL32(215, 100, 40, 255), IM_COL32(175, 48, 18, 255),
            IM_COL32(78, 18, 10, 255),   5.4f,
            IM_COL32(245, 125, 50, 255), 0.24f
        };
    case 4: /* Vivid Miku Turquoise -> Deep Ocean Teal */
        if (outAccent) *outAccent = IM_COL32(57, 215, 200, 255);
        return {
            IM_COL32(75, 245, 225, 255), IM_COL32(40, 200, 195, 255),
            IM_COL32(25, 175, 180, 255), IM_COL32(10, 118, 152, 255),
            IM_COL32(10, 30, 56, 255),   5.2f,
            IM_COL32(45, 210, 195, 255), 0.22f
        };
    default: /* Vivid Neon Violet -> Deep Royal Purple (5th..8th) */
        if (outAccent) *outAccent = IM_COL32(185, 115, 255, 255);
        return {
            IM_COL32(198, 132, 255, 255), IM_COL32(155, 90, 235, 255),
            IM_COL32(135, 70, 220, 255),  IM_COL32(88, 32, 178, 255),
            IM_COL32(28, 12, 62, 255),    5.2f,
            IM_COL32(165, 95, 255, 255),  0.22f
        };
    }
}

void DrawLeaderboard() {
    Snapshot s;
    GetSnapshot(&s);
    if (!s.inSong || OverlayOpen() || s.resultsAgeMs >= 0) {
        s_placeDispRank = 0; s_placePrevRank = 0; s_placeCandRank = 0;
        s_placeAnimAt = 0; s_placeEnterAt = 0; s_placeAvoidA = 1.0f;
        return;
    }
    int myRank = 0;
    for (int i = 0; i < s.boardCount; i++)
        if (s.board[i].me) {
            myRank = s.board[i].rank > 0 ? s.board[i].rank : (i + 1);
            break;
        }
    if (myRank <= 0) {
        if (s.phase == 4 /*L_PLAYING*/ && s.playerCount > 0) myRank = 1;
        else return;
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;
    const ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    float k = ds.y / 720.0f;
    float dt = io.DeltaTime > 0.0f ? io.DeltaTime : (1.0f / 60.0f);
    uint64_t now = _ms();
    float tSec = (float)ImGui::GetTime();

    /* track rank transitions (short 140 ms debounce against 1-frame tie jitter) */
    if (s_placeDispRank <= 0) {
        s_placeDispRank = myRank;
        s_placePrevRank = myRank;
        s_placeCandRank = 0;
        s_placeEnterAt  = now;
    } else if (myRank == s_placeDispRank) {
        s_placeCandRank = 0;
    } else if (s_placeCandRank != myRank) {
        s_placeCandRank = myRank;
        s_placeCandAt   = now;
    } else if (now - s_placeCandAt >= 140) {
        s_placePrevRank = s_placeDispRank;
        s_placeAnimUp   = myRank < s_placeDispRank ? 1 : 0;
        s_placeDispRank = myRank;
        s_placeCandRank = 0;
        s_placeAnimAt   = now;
    }

    /* fade when an active note target or flying button approaches top-right */
    float targetAvoid = s.topRightBusy ? 0.26f : 1.0f;
    float step = dt * 10.0f;
    if (step > 1.0f) step = 1.0f;
    s_placeAvoidA += (targetAvoid - s_placeAvoidA) * step;

    /* entrance animation (first 340 ms) */
    float enterSlideX = 0.0f, enterScale = 1.0f, enterAlpha = 1.0f;
    if (s_placeEnterAt > 0) {
        float ep = (float)(now - s_placeEnterAt) / 340.0f;
        if (ep < 1.0f) {
            if (ep < 0.0f) ep = 0.0f;
            enterSlideX = (1.0f - EaseOutBack(ep)) * 72.0f * k;
            enterScale  = 0.75f + 0.25f * EaseOutBack(ep);
            enterAlpha  = ep < 0.35f ? (ep / 0.35f) : 1.0f;
        }
    }

    /* rank-change 3D flip + elastic pop animation (650 ms total) */
    int shownRank = s_placeDispRank;
    float flipX = 1.0f, animScale = 1.0f, hopY = 0.0f, shakeX = 0.0f;
    float animAge = s_placeAnimAt > 0 ? (float)(now - s_placeAnimAt) : -1.0f;
    if (animAge >= 650.0f) { s_placeAnimAt = 0; animAge = -1.0f; }
    if (animAge >= 0.0f) {
        if (animAge < 110.0f) {
            /* phase 1: old rank rotates edge-on */
            float p = animAge / 110.0f;
            shownRank = s_placePrevRank > 0 ? s_placePrevRank : s_placeDispRank;
            flipX = cosf(p * 1.5707963f);
            if (flipX < 0.08f) flipX = 0.08f;
            animScale = 1.0f + 0.08f * p;
            hopY = (s_placeAnimUp ? -1.0f : 1.0f) * 3.5f * k * sinf(p * 1.5707963f);
        } else if (animAge < 540.0f) {
            /* phase 2: new rank flips open with elastic bounce or drop shake */
            float q = (animAge - 110.0f) / 430.0f;
            shownRank = s_placeDispRank;
            float fp = q < 0.32f ? (q / 0.32f) : 1.0f;
            flipX = sinf(fp * 1.5707963f);
            if (flipX < 0.08f) flipX = 0.08f;
            if (s_placeAnimUp) {
                animScale = q < 0.26f
                    ? (1.08f + 0.20f * (q / 0.26f))
                    : (1.0f + 0.28f * powf(1.0f - (q - 0.26f) / 0.74f, 2.0f) *
                              cosf((q - 0.26f) * 4.8f));
                hopY = -4.5f * k * (1.0f - q) * cosf(q * 2.4f);
            } else {
                animScale = q < 0.22f
                    ? (1.16f - 0.22f * (q / 0.22f))
                    : (0.94f + 0.06f * EaseOutBack((q - 0.22f) / 0.78f));
                hopY = 4.5f * k * sinf(q * 3.14159f) * (1.0f - q);
                shakeX = sinf(q * 31.4159f) * (1.0f - q) * 4.0f * k;
            }
        }
    }

    /* subtle continuous breathing pulse */
    float breathe = 1.0f + 0.020f * sinf(tSec * 3.8f);
    float glowPulse = 0.80f + 0.20f * sinf(tSec * 5.2f);
    float totalScale = enterScale * animScale * breathe;
    float alpha = 0.92f * enterAlpha * s_placeAvoidA;
    if (alpha <= 0.02f) return;

    /* top-right anchor directly beneath the 7-digit score (y ~ 10..38) */
    ImVec2 ctr(ds.x - 52.0f * k + enterSlideX, 70.0f * k);
    ImU32 accent = 0;
    GameTextStyle st = PlacementStyle(shownRank, &accent);
    st.glowA *= glowPulse;

    /* 1) sleek angled Project DIVA backing plate + accent trim */
    {
        float pw = 84.0f * k, ph = 33.0f * k, sk = 7.0f * k;
        ImVec2 pc(ctr.x + shakeX * 0.4f, ctr.y + 2.0f * k + hopY * 0.4f);
        ImVec2 q[4] = {
            ImVec2(pc.x - pw * 0.5f + sk, pc.y - ph * 0.5f),
            ImVec2(pc.x + pw * 0.5f + sk, pc.y - ph * 0.5f),
            ImVec2(pc.x + pw * 0.5f - sk, pc.y + ph * 0.5f),
            ImVec2(pc.x - pw * 0.5f - sk, pc.y + ph * 0.5f),
        };
        dl->AddConvexPolyFilled(q, 4, IM_COL32(12, 16, 32, (int)(135 * alpha)));
        /* diagonal DIVA speed stripes near the right edge */
        for (int sIdx = 0; sIdx < 2; sIdx++) {
            float ox = (26.0f + sIdx * 7.5f) * k;
            ImVec2 sp[4] = {
                ImVec2(pc.x + ox + sk,            pc.y - ph * 0.5f),
                ImVec2(pc.x + ox + 4.0f * k + sk, pc.y - ph * 0.5f),
                ImVec2(pc.x + ox + 4.0f * k - sk, pc.y + ph * 0.5f),
                ImVec2(pc.x + ox - sk,            pc.y + ph * 0.5f),
            };
            dl->AddConvexPolyFilled(sp, 4,
                (accent & 0x00ffffff) | ((ImU32)(int)(55 * alpha) << 24));
        }
        /* bottom + right glowing bevel trim */
        ImU32 trimC = (accent & 0x00ffffff) | ((ImU32)(int)(225 * alpha) << 24);
        dl->AddLine(q[3], q[2], trimC, 2.4f * k);
        dl->AddLine(q[2], q[1], trimC, 2.4f * k);
        dl->AddLine(q[0], q[1], IM_COL32(255, 255, 255, (int)(70 * alpha)), 1.2f * k);
    }

    /* 2) rank-change shockwave ring + rising/falling DIVA chevrons */
    if (animAge >= 0.0f) {
        ImU32 evCol = s_placeAnimUp ? IM_COL32(95, 255, 165, 255)
                                    : IM_COL32(205, 105, 255, 255);
        if (animAge < 460.0f) {
            float rp = animAge / 460.0f;
            float expand = 1.0f + 0.65f * (1.0f - (1.0f - rp) * (1.0f - rp));
            int ra = (int)((1.0f - rp) * (1.0f - rp) * 220.0f * alpha);
            if (ra > 0) {
                float rw = 44.0f * k * expand, rh = 19.0f * k * expand, rsk = 6.0f * k * expand;
                ImVec2 rq[4] = {
                    ImVec2(ctr.x - rw + rsk, ctr.y - rh),
                    ImVec2(ctr.x + rw + rsk, ctr.y - rh),
                    ImVec2(ctr.x + rw - rsk, ctr.y + rh),
                    ImVec2(ctr.x - rw - rsk, ctr.y + rh),
                };
                dl->AddPolyline(rq, 4, (evCol & 0x00ffffff) | ((ImU32)ra << 24),
                                ImDrawFlags_Closed, (3.2f * (1.0f - rp) + 1.0f) * k);
            }
        }
        /* double directional chevrons on the left of the badge */
        float cp = animAge / 650.0f;
        float cAlpha = (cp < 0.15f ? cp / 0.15f : 1.0f - (cp - 0.15f) / 0.85f) * alpha;
        if (cAlpha > 0.01f) {
            float dir = s_placeAnimUp ? -1.0f : 1.0f;
            float slide = dir * (cp * 14.0f - 5.0f) * k;
            float cx = ctr.x - 50.0f * k;
            ImU32 cc = (evCol & 0x00ffffff) | ((ImU32)(int)(245 * cAlpha) << 24);
            ImU32 co = IM_COL32(12, 16, 32, (int)(210 * cAlpha));
            for (int cIdx = 0; cIdx < 2; cIdx++) {
                float cy = ctr.y + slide + (cIdx - 0.5f) * 8.5f * k;
                float cw = 6.5f * k;
                float tipY  = cy + dir * 4.5f * k;
                float wingY = cy - dir * 4.5f * k;
                ImVec2 pts[3] = {
                    ImVec2(cx - cw, wingY),
                    ImVec2(cx,      tipY),
                    ImVec2(cx + cw, wingY),
                };
                dl->AddPolyline(pts, 3, co, 0, 4.2f * k);
                dl->AddPolyline(pts, 3, cc, 0, 2.4f * k);
            }
        }
    }

    /* 3) Mario-Kart style numeral + ordinal suffix with unified strokes & gloss */
    ImFont* f = FontLoader::Big() ? FontLoader::Big() : ImGui::GetFont();
    char numTxt[8];
    snprintf(numTxt, sizeof(numTxt), "%d", shownRank);
    const char* sufTxt = OrdinalSuffix(shownRank);
    float szNum = 46.0f * k, szSuf = 24.5f * k;
    ImVec2 tsNum = f->CalcTextSizeA(szNum, FLT_MAX, 0.0f, numTxt);
    ImVec2 tsSuf = f->CalcTextSizeA(szSuf, FLT_MAX, 0.0f, sufTxt);
    float gapX = 2.2f * k;
    float totalW = tsNum.x + gapX + tsSuf.x;
    ImVec2 pNum(ctr.x - totalW * 0.5f, ctr.y - tsNum.y * 0.5f);
    ImVec2 pSuf(pNum.x + tsNum.x + gapX,
                pNum.y + 0.78f * (tsNum.y - tsSuf.y) - 1.0f * k);

    float whiteRNum = 2.0f * k, whiteRSuf = 1.6f * k;
    float darkRNum  = 3.8f * k, darkRSuf  = 3.0f * k;
    int vtx0 = dl->VtxBuffer.Size;

    auto ringBoth = [&](ImVec2 dOff, float rNum, float rSuf, int n, ImU32 col, int a) {
        ImU32 c = (col & 0x00ffffff) | ((ImU32)a << 24);
        for (int i = 0; i < n; i++) {
            float th = i * 6.2831853f / n;
            float cs = cosf(th), sn = sinf(th);
            dl->AddText(f, szNum,
                        ImVec2(pNum.x + dOff.x + cs * rNum, pNum.y + dOff.y + sn * rNum),
                        c, numTxt);
            dl->AddText(f, szSuf,
                        ImVec2(pSuf.x + dOff.x + cs * rSuf, pSuf.y + dOff.y + sn * rSuf),
                        c, sufTxt);
        }
    };

    /* pass 1: drop shadow */
    ringBoth(ImVec2(3.0f * k, 4.0f * k), darkRNum, darkRSuf, 16,
             st.dark, (int)(0.52f * 255));
    /* pass 2: soft outer glow */
    if (st.glowA > 0.0f)
        ringBoth(ImVec2(0, 0), 5.6f * k, 4.4f * k, 16,
                 st.glow, (int)(st.glowA * 255));
    /* pass 3: unified dark outer stroke */
    ringBoth(ImVec2(0, 0), darkRNum, darkRSuf, 16, st.dark, 255);
    /* pass 4: crisp white inner keyline */
    int vtxWhite = dl->VtxBuffer.Size;
    ringBoth(ImVec2(0, 0), whiteRNum, whiteRSuf, 12,
             IM_COL32(255, 255, 255, 255), 255);

    /* pass 5: rich top-to-bottom vertical gradient fill on numeral & suffix
     * (like GlowText on the results screen; each glyph quad has vertices at
     * t=0 top and t=1 bottom, so LerpCol(st.top, st.bot, t) gives a crisp,
     * saturated gradient with no white filter) */
    auto fillGlyph = [&](ImVec2 gp, float gsz, const char* gtxt) {
        int vf0 = dl->VtxBuffer.Size;
        dl->AddText(f, gsz, gp, IM_COL32(255, 255, 255, 255), gtxt);
        int vf1 = dl->VtxBuffer.Size;
        float yT = 1e30f, yB = -1e30f;
        for (int v = vf0; v < vf1; v++) {
            float vy = dl->VtxBuffer[v].pos.y;
            if (vy < yT) yT = vy;
            if (vy > yB) yB = vy;
        }
        for (int v = vf0; v < vf1; v++) {
            ImDrawVert& vt = dl->VtxBuffer[v];
            float t = yB > yT ? (vt.pos.y - yT) / (yB - yT) : 0.0f;
            ImU32 c = LerpCol(st.top, st.bot, t);
            vt.col = (c & 0x00ffffffu) | (vt.col & 0xff000000u);
        }
    };
    fillGlyph(pNum, szNum, numTxt);
    fillGlyph(pSuf, szSuf, sufTxt);
    int vtxEnd = dl->VtxBuffer.Size;

    /* transform all passes: italic shear, 3D flip + scale + hop, alpha */
    const float italic = 0.20f;
    int aMul = (int)(alpha * 255.0f);
    for (int i = vtx0; i < vtxEnd; i++) {
        ImDrawVert& v = dl->VtxBuffer[i];
        float dx = (v.pos.x - ctr.x) + (ctr.y - v.pos.y) * italic;
        float dy = (v.pos.y - ctr.y);
        v.pos.x = ctr.x + dx * flipX * totalScale + shakeX;
        v.pos.y = ctr.y + dy * totalScale + hopY;
        ImU32 va = (v.col >> 24) & 0xff;
        v.col = (v.col & 0x00ffffff) | ((ImU32)((va * aMul) / 255) << 24);
    }

    /* 4) Project DIVA sparkles: twinkling on 1st place + burst on rank-up */
    if (shownRank == 1) {
        float sp1 = 0.55f + 0.45f * sinf(tSec * 5.5f);
        float sp2 = 0.55f + 0.45f * sinf(tSec * 4.3f + 2.1f);
        Sparkle(dl, ImVec2(ctr.x - 28.0f * k + shakeX, ctr.y - 17.0f * k + hopY),
                (6.5f + 3.0f * sp1) * k,
                IM_COL32(255, 255, 235, (int)(240 * alpha * sp1)));
        Sparkle(dl, ImVec2(ctr.x + 32.0f * k + shakeX, ctr.y + 13.0f * k + hopY),
                (5.0f + 2.5f * sp2) * k,
                IM_COL32(255, 235, 160, (int)(225 * alpha * sp2)));
    }
    if (animAge >= 100.0f && animAge < 520.0f && s_placeAnimUp) {
        float bp = (animAge - 100.0f) / 420.0f;
        float bAlpha = (1.0f - bp) * alpha;
        float rad = (22.0f + 24.0f * bp) * k;
        static const float kAng[4] = { -2.4f, -0.55f, 0.75f, 2.35f };
        for (int sIdx = 0; sIdx < 4; sIdx++) {
            ImVec2 sp(ctr.x + cosf(kAng[sIdx]) * rad * 1.25f,
                      ctr.y + sinf(kAng[sIdx]) * rad * 0.78f);
            Sparkle(dl, sp, (7.5f * (1.0f - bp * 0.6f)) * k,
                    IM_COL32(255, 252, 210, (int)(245 * bAlpha)));
        }
    }
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

/* ================================================================
 * Song select: "doesn't have this song" tag
 * ============================================================== */
/* A slanted plate in the DIVA warm gradient under the "picking a song"
 * pill, for everyone in the room: who lacks the picker's song (portraits
 * on the right), or that this console does. Right edge at rx, top at y;
 * returns the height used (0 when everybody has the song). */
float DrawNoSongTag(ImDrawList* dl, ImFont* font, float rx, float y, float s) {
    static uint64_t s_shownAt = 0;      /* ms the tag appeared (slide-in) */
    Snapshot sn;
    GetSnapshot(&sn);
    int idx[8], n = 0;
    bool me = false;
    if (sn.connected && sn.code[0])
        for (int i = 0; i < sn.memberCount && n < 8; i++)
            if (sn.members[i].nosong) {
                if (strcmp(sn.members[i].cid, sn.myCid) == 0) me = true;
                idx[n++] = i;
            }
    if (n == 0) { s_shownAt = 0; return 0.0f; }
    const uint64_t now = NavNowMs();
    if (!s_shownAt) s_shownAt = now;
    float p = (float)(now - s_shownAt) / 220.0f;
    if (p > 1.0f) p = 1.0f;
    const float ease = 1.0f - (1.0f - p) * (1.0f - p);
    const float alpha = ease;
    const float t = (float)ImGui::GetTime();
    const float pulse = 0.5f + 0.5f * sinf(t * 6.28318f * 0.9f);

    const float fs1 = font->FontSize * 1.2f, fs2 = font->FontSize * 0.98f;
    const float maxText = 320.0f * s;
    auto width = [&](const char* txt, float fsz) {
        return font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, txt).x;
    };
    char l1[112], l2[112];
    const bool meLeader = strcmp(sn.leaderCid, sn.myCid) == 0;
    /* our selector can't show the song, so name it: "<song> - you'll sit
     * this round out", trimming the name, never the message */
    auto mine = [&]() {
        const char* nm = sn.selName;
        int len = (int)strlen(nm);
        if (!len) { snprintf(l2, sizeof(l2), "You'll sit this round out"); return; }
        for (;;) {
            snprintf(l2, sizeof(l2), "%.*s%s - you'll sit this round out", len, nm,
                     len < (int)strlen(nm) ? ".." : "");
            if (len <= 4 || width(l2, fs2) <= maxText) break;
            do { len--; } while (len > 0 && ((unsigned char)nm[len] & 0xC0) == 0x80);
        }
    };
    if (me && n == 1) {
        snprintf(l1, sizeof(l1), "You don't have this song");
        mine();
    } else if (me) {
        snprintf(l1, sizeof(l1), "You and %d other%s don't have this song", n - 1, n == 2 ? "" : "s");
        mine();
    } else if (n == 1) {
        /* trim the name, never the message (UTF-8 safe) */
        const char* nm = sn.members[idx[0]].name;
        int len = (int)strlen(nm);
        for (;;) {
            snprintf(l1, sizeof(l1), "%.*s%s doesn't have this song", len, nm,
                     len < (int)strlen(nm) ? ".." : "");
            if (len == 0 || width(l1, fs1) <= maxText) break;
            do { len--; } while (len > 0 && ((unsigned char)nm[len] & 0xC0) == 0x80);
        }
        snprintf(l2, sizeof(l2), "%s", meLeader ? "They'll sit out - or pick another song"
                                                : "They'll sit this round out");
    } else {
        snprintf(l1, sizeof(l1), "%d players don't have this song", n);
        int off = 0;
        for (int i = 0; i < n && off < (int)sizeof(l2) - 44; i++)
            off += snprintf(l2 + off, sizeof(l2) - off, "%s%s", i ? ", " : "", sn.members[idx[i]].name);
        int len = (int)strlen(l2);
        while (len > 0 && width(l2, fs2) > maxText) {
            do { len--; } while (len > 0 && ((unsigned char)l2[len] & 0xC0) == 0x80);
            snprintf(l2 + len, sizeof(l2) - len, "..");
        }
    }
    ImVec2 t1 = font->CalcTextSizeA(fs1, FLT_MAX, 0.0f, l1);
    ImVec2 t2 = font->CalcTextSizeA(fs2, FLT_MAX, 0.0f, l2);
    const float textW = t1.x > t2.x ? t1.x : t2.x;

    /* portraits of the players without the song (not our own) */
    int av[3], nav = 0;
    for (int i = 0; i < n && nav < 3; i++)
        if (strcmp(sn.members[idx[i]].cid, sn.myCid) != 0) av[nav++] = idx[i];
    const float ar = 13.0f * s, astep = 19.0f * s;
    const float avW = nav ? 2 * ar + (nav - 1) * astep + 8.0f * s : 0.0f;

    const float padX = 10.0f * s, iconR = 12.0f * s;
    const float h = 6.0f * s + t1.y + 3.0f * s + t2.y + 6.0f * s;
    const float sk = h * 0.22f;
    const float w = sk + padX + 2 * iconR + 8.0f * s + textW + avW + padX + sk * 0.5f;
    const float xoff = (1.0f - ease) * 36.0f * s;
    ImVec2 a(rx - w + xoff, y), b(rx + xoff, y + h);

    /* plate: drop shadow, warm gradient, sheen, white keyline, pulsing rim */
    SlantPlate(dl, ImVec2(a.x + 3 * s, a.y + 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s), sk,
               IM_COL32(0, 0, 0, (int)(110 * alpha)), IM_COL32(0, 0, 0, (int)(110 * alpha)));
    SlantPlate(dl, a, b, sk, IM_COL32(255, 122, 66, (int)(246 * alpha)),
               IM_COL32(238, 48, 128, (int)(246 * alpha)));
    SlantPlate(dl, a, ImVec2(b.x - sk * 0.55f, a.y + h * 0.45f), sk * 0.45f,
               IM_COL32(255, 255, 255, (int)(46 * alpha)), IM_COL32(255, 255, 255, (int)(8 * alpha)));
    SlantOutline(dl, ImVec2(a.x - 2.5f * s, a.y - 2.5f * s), ImVec2(b.x + 2.5f * s, b.y + 2.5f * s), sk,
                 IM_COL32(255, 96, 150, (int)((50 + 110 * pulse) * alpha)), 2.5f * s);
    SlantOutline(dl, a, b, sk, IM_COL32(255, 255, 255, (int)(235 * alpha)), 1.6f * s);

    /* "!" disc */
    const float my = (a.y + b.y) * 0.5f;
    ImVec2 ic(a.x + sk + padX + iconR, my);
    const float ir = iconR * (1.0f + 0.07f * pulse);
    dl->AddCircleFilled(ic, ir, IM_COL32(255, 255, 255, (int)(255 * alpha)), 24);
    const float es = font->FontSize * 1.35f;
    ImVec2 ex = font->CalcTextSizeA(es, FLT_MAX, 0.0f, "!");
    dl->AddText(font, es, ImVec2(ic.x - ex.x * 0.5f, ic.y - ex.y * 0.5f),
                IM_COL32(236, 44, 120, (int)(255 * alpha)), "!");
    dl->AddText(font, es, ImVec2(ic.x - ex.x * 0.5f + 0.7f * s, ic.y - ex.y * 0.5f),
                IM_COL32(236, 44, 120, (int)(255 * alpha)), "!");

    /* two lines, soft dark shadow for contrast on the warm plate */
    const float tx = ic.x + iconR + 8.0f * s;
    const float ty = a.y + 6.0f * s;
    const ImU32 sh = IM_COL32(110, 12, 50, (int)(170 * alpha));
    dl->AddText(font, fs1, ImVec2(tx + 1 * s, ty + 1 * s), sh, l1);
    dl->AddText(font, fs1, ImVec2(tx, ty), IM_COL32(255, 255, 255, (int)(255 * alpha)), l1);
    dl->AddText(font, fs2, ImVec2(tx + 1 * s, ty + t1.y + 4 * s), sh, l2);
    dl->AddText(font, fs2, ImVec2(tx, ty + t1.y + 3 * s), IM_COL32(255, 238, 214, (int)(255 * alpha)), l2);

    if (nav && alpha > 0.6f) {
        Ui u;
        u.dl = dl; u.font = font; u.k = s; u.fs = font->FontSize;
        u.mouse = ImVec2(-1, -1); u.clicked = false; u.down = false; u.t = t;
        float ax = b.x - padX - sk * 0.5f - ar - (nav - 1) * astep;
        for (int i = 0; i < nav; i++) {
            const MemberInfo& m = sn.members[av[i]];
            DrawAvatar(u, ImVec2(ax + i * astep, my), ar, MemberRef(sn, m), false, m.connected, false);
        }
    }
    return h + 5.0f * s;
}

} // namespace mp::lobby
