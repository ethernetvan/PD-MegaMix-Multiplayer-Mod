#include "MpLobby.hpp"
#include "MpAvatar.hpp"
#include "MpInput.hpp"
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
 *
 * In a room the overlay always opens straight on ROOM.
 * ============================================================== */

enum Screen { SC_HOME, SC_EDIT_NAME, SC_JOIN_ROOM, SC_ROOM, SC_ROOM_CONFIG };

static Screen s_screen   = SC_HOME;
static char   s_codeBuf[8]  = "";
static char   s_nameBuf[40] = "";
static bool   s_kbArmed  = false;   /* keyboard was opened for this screen */
static bool   s_prevOpen = false;

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

static void DrawAvatar(const Ui& u, ImVec2 c, float r, int32_t module, bool you, bool connected,
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
    if (mp::avatar::Get(module, &tex, uv0, uv1) == 1) {
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
static void DrawResultHex(const Ui& u, ImVec2 c, float r, int32_t module,
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
    } else if (mp::avatar::Get(module, &tex, uv0, uv1) == 1) {
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
static int32_t StandingModule(const Snapshot& sn, int i) {
    int32_t local = mp::avatar::LocalModule();
    return (sn.standing[i].me && local >= 0) ? local : sn.standing[i].module;
}

static float EaseOutBack(float p) {
    const float c1 = 1.70158f;
    float q = p - 1.0f;
    return 1.0f + (c1 + 1.0f) * q * q * q + c1 * q * q;
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
            DrawResultHex(u, c, r720 * k, StandingModule(sn, ord[m]), st.me,
                          st.connected, st.final,
                          i == 0 && m == 0,          /* top group sparkles */
                          shown > 1 ? 0.0f : 0.15f,
                          cnt >= 4 && m == 2);       /* "+" = the others */
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

/* ---- helpers ---- */
static const char* MemberStatus(const MemberInfo& m) {
    if (!m.connected) return "disconnected";
    if (m.ready) return "ready";
    if (strcmp(m.reason, "modules") == 0) return "choosing modules";
    if (strcmp(m.reason, "wrong_song") == 0) return "wrong song";
    if (strcmp(m.reason, "difficulty") == 0) return "difficulty";
    if (strcmp(m.reason, "not_in_sel") == 0) return "not in song select";
    return "not ready";
}

static ImU32 MemberDot(const MemberInfo& m) {
    return !m.connected ? kDim : (m.ready ? kGreen : kYellow);
}

/* your own avatar follows the save immediately; others come from the server */
static int32_t MemberModule(const Snapshot& s, const MemberInfo& m) {
    int32_t local = mp::avatar::LocalModule();
    return (local >= 0 && strcmp(m.cid, s.myCid) == 0) ? local : m.module;
}

static void GoHomeOrRoom() {
    s_screen = InLobby() ? SC_ROOM : SC_HOME;
    s_kbArmed = false;
    if (VirtualKeyboard::IsOpen()) VirtualKeyboard::Close();
}

static void OnNameSubmit() {
    if (s_nameBuf[0]) CmdSetName(s_nameBuf);
    s_screen = SC_HOME; s_kbArmed = false;
}
static void OnCodeSubmit() {
    if (s_codeBuf[0]) CmdLobbyJoin(s_codeBuf);
    s_screen = SC_HOME; s_kbArmed = false;
}

static void OpenEditName() {
    strncpy(s_nameBuf, Config::mpPlayerName.c_str(), sizeof(s_nameBuf) - 1);
    s_nameBuf[sizeof(s_nameBuf) - 1] = 0;
    s_screen = SC_EDIT_NAME;
    VirtualKeyboard::Open(s_nameBuf, sizeof(s_nameBuf), "SAVE", OnNameSubmit);
    s_kbArmed = true;
}
static void OpenJoinRoom() {
    s_codeBuf[0] = 0;
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

    /* footer pill: MULTIPLAYER: state */
    const char* st = !Config::mpEnabled ? "MULTIPLAYER: OFF"
                   : (online ? "MULTIPLAYER: CONNECTED" : "MULTIPLAYER: DISCONNECTED");
    ImU32 stc = !Config::mpEnabled ? kDim : (online ? kGreen : kRed);
    float pillH = 40 * u.k;
    ImVec2 ps = u.Size(st, 1.3f);
    float pw = ps.x + 44 * u.k;
    ImVec2 pa((a.x + b.x - pw) * 0.5f, b.y - 24 * u.k - pillH), pb(pa.x + pw, pa.y + pillH);
    u.dl->AddRectFilled(pa, pb, (stc & 0x00FFFFFF) | (40u << 24), pillH * 0.5f);
    u.dl->AddRect(pa, pb, stc, pillH * 0.5f, 0, 1.4f);
    u.TextCentered(pa, pb, st, 1.3f, stc);

    /* Notifications segmented control: Avatars | Text | Off */
    {
        int st = NotifyStyle();
        const char* seg[3] = { "Avatars", "Text", "Off" };
        float sw = 118 * u.k, sh = 38 * u.k, sx0 = a.x + 30 * u.k;
        u.TextLeft(ImVec2(sx0, top + 24 * u.k),
                   ImVec2(sx0 + 190 * u.k, top + 24 * u.k + sh),
                   "Notifications:", 1.25f, kMuted);
        for (int i = 0; i < 3; i++) {
            ImVec2 sa(sx0 + 190 * u.k + i * (sw + 4 * u.k), top + 24 * u.k),
                   sb(sa.x + sw, sa.y + sh);
            int fi = u.Focus(sa, sb, true);
            bool hov = u.FHover(fi), cur = st == i;
            u.dl->AddRectFilled(sa, sb,
                cur ? kAccent : (hov ? kCardHov : kCard), 10 * u.k);
            if (hov) u.Ring(sa, sb, 10 * u.k);
            else u.dl->AddRect(sa, sb, kLine, 10 * u.k, 0, 1.2f);
            u.TextCentered(sa, sb, seg[i], 1.3f,
                           cur ? IM_COL32(20, 20, 20, 255) : kText);
            if (u.FActivated(fi)) { sound::PlayEnterSE(); SetNotifyStyle(i); }
        }
    }

    /* three cards */
    float gap = 22 * u.k;
    float cy0 = top + 84 * u.k, cy1 = pa.y - 22 * u.k;
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
        if (u.FActivated(fi)) {
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
    /* how to get going, right of the pill */
    const char* tip = !Config::mpEnabled ? "Set enabled = true in config.toml, then restart"
                    : (!online ? "Looking for the server..." : nullptr);
    if (tip) {
        char tb[96];
        u.Fit(tip, b.x - 30 * u.k - pb.x - 16 * u.k, 1.15f, tb, sizeof(tb));
        ImVec2 tsz = u.Size(tb, 1.15f);
        u.Text(ImVec2(b.x - 30 * u.k - tsz.x, pa.y + (pillH - tsz.y) * 0.5f), tb, 1.15f, kMuted);
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
                           void (*onOk)(), const char* help) {
    float top = Header(u, a, b, title, false);

    /* field */
    float fh = 70 * u.k;
    ImVec2 fa(a.x + 30 * u.k, top + 28 * u.k), fb(b.x - 30 * u.k, fa.y + fh);
    u.dl->AddRectFilled(fa, fb, kField, 10 * u.k);
    u.dl->AddRect(fa, fb, kAccent, 10 * u.k, 0, 2.0f * u.k);
    char shown[48];
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
    char pre[48]; memcpy(pre, shown, cp); pre[cp] = 0;
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
    if (u.Button(oa, ob, okLabel, Ui::B_PRIMARY, 1.55f, buf[0] != 0, false)) {
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
    float splitX = a.x + (b.x - a.x) * 0.58f;

    /* ---- podium: first three members in join order ---- */
    int n = s.memberCount < 3 ? s.memberCount : 3;
    float colW = (splitX - a.x - 30 * u.k) / 3.0f;
    float rads[3]  = { 82 * u.k, 68 * u.k, 58 * u.k };
    float plateW[3] = { 76 * u.k, 64 * u.k, 56 * u.k };
    /* avatar bottoms align here; the whole podium block (~2r + plate +
     * name) sits around the vertical middle of the content area */
    float baseY = (cy0 + cy1) * 0.5f + 12 * u.k;
    for (int i = 0; i < n; i++) {
        const MemberInfo& m = s.members[i];
        float cx = a.x + 30 * u.k + colW * (i + 0.5f);
        float r = rads[i];
        ImVec2 ac(cx, baseY - r);
        bool you = strcmp(m.cid, s.myCid) == 0;
        DrawAvatar(u, ac, r, MemberModule(s, m), you, m.connected, true);
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
        u.Fit(m.name, colW - 6 * u.k, nscale, nm, sizeof(nm));
        ImVec2 nsz = u.Size(nm, nscale);
        u.Text(ImVec2(cx - nsz.x * 0.5f, ny), nm, nscale, m.connected ? kText : kDim);
        char cap[48];
        RoleCaption(s, m, cap, sizeof(cap));
        if (cap[0]) {
            ImVec2 csz = u.Size(cap, 1.2f);
            u.Text(ImVec2(cx - csz.x * 0.5f, ny + nsz.y + 3 * u.k), cap, 1.2f, kMuted);
        }
    }
    if (n == 0)
        u.TextCentered(ImVec2(a.x, cy0), ImVec2(splitX, cy1), "Waiting for the room list...", 1.45f, kMuted);

    /* ---- ordered list with kick buttons ---- */
    float lx = splitX + 10 * u.k;
    float rowH = 56 * u.k;
    float ly = cy0 + 6 * u.k;
    bool assignMode = owner && strcmp(s.pickMode, "assigned") == 0;
    for (int i = 0; i < s.memberCount; i++) {
        const MemberInfo& m = s.members[i];
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
        DrawAvatar(u, hc, hr, MemberModule(s, m), false, m.connected, false);
        ImVec2 dc(hc.x + hr * 0.66f, hc.y + hr * 0.66f);
        u.dl->AddCircleFilled(dc, 6 * u.k, MemberDot(m), 16);
        u.dl->AddCircle(dc, 6 * u.k, kBg, 16, 2.0f * u.k);
        float nameX = ra.x + 94 * u.k;
        float nameR = rb.x - (owner && !you ? 64 * u.k : 8 * u.k);
        u.TextLeft(ImVec2(nameX, ra.y), ImVec2(nameR, rb.y), m.name, 1.6f,
                   m.connected ? kText : kDim);
        if (lead) {
            /* leader tag right after the name */
            ImVec2 nsz = u.Size(m.name, 1.6f);
            float tx = nameX + nsz.x + 12 * u.k;
            float tw = u.Size("LEADER", 0.95f).x + 16 * u.k;
            if (tx + tw + 4 * u.k < nameR) {
                float my = (ra.y + rb.y) * 0.5f;
                ImVec2 ta(tx, my - 11 * u.k), tb(tx + tw, my + 11 * u.k);
                u.dl->AddRectFilled(ta, tb, kGreenBg, 11 * u.k);
                u.dl->AddRect(ta, tb, kGreen, 11 * u.k, 0, 1.0f);
                u.TextCentered(ta, tb, "LEADER", 0.95f, kGreen);
            }
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
    else if (s.notReady[0])
        snprintf(foot, sizeof(foot), "Not ready: %s", s.notReady);
    else if (s.statusLine[0] && s.phase != 1 /* SELECT: leader can change, build it live */)
        snprintf(foot, sizeof(foot), "%s", s.statusLine);
    else {
        const char* mode = strcmp(s.pickMode, "owner") == 0 ? "Owner picks"
                         : (strcmp(s.pickMode, "assigned") == 0 ? "Assigned" : "Rotate");
        if (strcmp(s.leaderCid, s.myCid) == 0)
            snprintf(foot, sizeof(foot), "You're picking the song  |  %s", mode);
        else
            snprintf(foot, sizeof(foot), "%s is picking the song  |  %s",
                     leadName[0] ? leadName : "The leader", mode);
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

/* ---------------------------------------------------------- ROOM CONFIG */
static void DrawRoomConfig(const Ui& u, const Snapshot& s, ImVec2 a, ImVec2 b) {
    float top = Header(u, a, b, "ROOM CONFIG", false);
    float bh = 48 * u.k, bw = 150 * u.k;
    ImVec2 ka(b.x - 30 * u.k - bw, a.y + (66 * u.k - bh) * 0.5f), kb(ka.x + bw, ka.y + bh);
    if (u.Button(ka, kb, "Done", Ui::B_PRIMARY, 1.4f)) {
        sound::PlayCancelSE();
        s_screen = SC_ROOM;
    }

    float y = top + 22 * u.k;
    u.Text(ImVec2(a.x + 30 * u.k, y), "Who picks the song", 1.6f, kText);
    y += 34 * u.k;
    u.Text(ImVec2(a.x + 30 * u.k, y), "Applies from the next round if a song is already running.", 1.15f, kMuted);
    y += 32 * u.k;

    struct Opt { const char* mode; const char* label; const char* desc; } opts[3] = {
        { "rotate",   "ROTATE",     "Everyone takes turns, one song each" },
        { "owner",    "OWNER",      "The room owner always picks" },
        { "assigned", "ASSIGNED",   "One chosen member picks every song" } };
    float ow = (b.x - a.x - 60 * u.k - 2 * 18 * u.k) / 3.0f, oh = 104 * u.k;
    for (int i = 0; i < 3; i++) {
        ImVec2 oa(a.x + 30 * u.k + i * (ow + 18 * u.k), y), ob(oa.x + ow, oa.y + oh);
        bool sel = strcmp(s.pickMode, opts[i].mode) == 0;
        int fi = u.Focus(oa, ob);
        bool hov = u.FHover(fi);
        u.dl->AddRectFilled(oa, ob, sel ? kAccentBg : (hov ? kCardHov : kCard), 12 * u.k);
        u.dl->AddRect(oa, ob, (sel || hov) ? kAccent : kLine, 12 * u.k, 0, (sel || hov) ? 2.5f * u.k : 1.2f);
        if (hov && s_padMode) u.Ring(oa, ob, 12 * u.k);
        /* radio */
        ImVec2 rc(oa.x + 24 * u.k, oa.y + 30 * u.k);
        u.dl->AddCircle(rc, 11 * u.k, sel ? kAccent : kMuted, 24, 2.0f * u.k);
        if (sel) u.dl->AddCircleFilled(rc, 6 * u.k, kAccent, 24);
        ImVec2 ls = u.Size(opts[i].label, 1.6f);
        u.Text(ImVec2(oa.x + 44 * u.k, rc.y - ls.y * 0.5f), opts[i].label, 1.6f, kText);
        char desc[96];
        u.Fit(opts[i].desc, ow - 28 * u.k, 1.15f, desc, sizeof(desc));
        u.Text(ImVec2(oa.x + 14 * u.k, oa.y + 62 * u.k), desc, 1.15f, kMuted);
        if (u.FActivated(fi) && !sel) {
            sound::PlaySelectSE();
            CmdPickMode(opts[i].mode, i == 2 ? s.myCid : nullptr);
        }
    }
    y += oh + 22 * u.k;

    /* assignee list (only meaningful in ASSIGNED) */
    bool assigned = strcmp(s.pickMode, "assigned") == 0;
    u.Text(ImVec2(a.x + 30 * u.k, y), assigned ? "Assigned picker" : "Members", 1.6f, kText);
    y += 34 * u.k;
    float rowH = 48 * u.k, colW = (b.x - a.x - 60 * u.k - 18 * u.k) * 0.5f;
    for (int i = 0; i < s.memberCount; i++) {
        const MemberInfo& m = s.members[i];
        int col = i % 2, row = i / 2;
        ImVec2 ra(a.x + 30 * u.k + col * (colW + 18 * u.k), y + row * rowH), rb(ra.x + colW, ra.y + rowH - 6 * u.k);
        if (rb.y > b.y - 50 * u.k) break;
        bool isPicker = strcmp(m.cid, s.leaderCid) == 0;
        int fi = u.Focus(ra, rb, assigned);
        bool hov = assigned && u.FHover(fi);
        u.dl->AddRectFilled(ra, rb, isPicker ? kAccentBg : (hov ? kCardHov : kCard), 8 * u.k);
        u.dl->AddRect(ra, rb, (isPicker || hov) ? kAccent : kLine, 8 * u.k, 0, 1.2f);
        if (hov && s_padMode) u.Ring(ra, rb, 8 * u.k);
        u.dl->AddCircleFilled(ImVec2(ra.x + 18 * u.k, (ra.y + rb.y) * 0.5f), 6 * u.k, MemberDot(m), 16);
        char line[64];
        snprintf(line, sizeof(line), "%s%s", m.name, strcmp(m.cid, s.myCid) == 0 ? " (You)" : "");
        u.TextLeft(ImVec2(ra.x + 34 * u.k, ra.y), ImVec2(rb.x - 90 * u.k, rb.y), line, 1.4f,
                   m.connected ? kText : kDim);
        if (isPicker) {
            ImVec2 ps = u.Size("PICKER", 1.05f);
            u.Text(ImVec2(rb.x - 14 * u.k - ps.x, (ra.y + rb.y - ps.y) * 0.5f), "PICKER", 1.05f, kAccent);
        }
        if (assigned && u.FActivated(fi) && !isPicker) {
            sound::PlaySelectSE();
            CmdPickMode("assigned", m.cid);
        }
    }
    const GuideEnt g[] = {{"A", "OK"}, {"B", "Back"}};
    float gw = GuideW(u, g, 2);
    DrawGuide(u, ImVec2(b.x - 30 * u.k - gw, a.y), b, g, 2);
    Footer(u, a, b, assigned ? "Tap a member to make them the song picker"
                             : "Pick ASSIGNED to choose a fixed song picker",
           gw + 10 * u.k);
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
    if (!open && s_prevOpen) {
        if (VirtualKeyboard::IsOpen() && s_kbArmed) VirtualKeyboard::Close();
        VirtualKeyboard::SetPadMode(false);
        s_armCid[0] = 0;
    }
    s_prevOpen = open;
    if (!open) return;

    Snapshot s;
    GetSnapshot(&s);

    /* screen fix-ups driven by lobby state */
    bool inLobby = InLobby();
    if (inLobby && (s_screen == SC_HOME || s_screen == SC_JOIN_ROOM)) GoHomeOrRoom();
    if (!inLobby && (s_screen == SC_ROOM || s_screen == SC_ROOM_CONFIG)) GoHomeOrRoom();
    if (s_screen == SC_ROOM_CONFIG && (strcmp(s.ownerCid, s.myCid) != 0 || !s.connected))
        s_screen = SC_ROOM;
    /* keyboard dismissed (CLOSE / tap outside) while editing -> cancel */
    if ((s_screen == SC_EDIT_NAME || s_screen == SC_JOIN_ROOM) && s_kbArmed &&
        !VirtualKeyboard::IsOpen())
        GoHomeOrRoom();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize;
    float k = ds.y / 720.0f;
    bool textScreen = s_screen == SC_EDIT_NAME || s_screen == SC_JOIN_ROOM;

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
            s_lastScreen = s_screen;
            s_armCid[0] = 0;
            if (s_screen == SC_HOME)
                s_focusIdx = s.connected ? 2 : 1;   /* [X, EDIT, CREATE, JOIN] */
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
                const char* buf = (s_screen == SC_EDIT_NAME) ? s_nameBuf : s_codeBuf;
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
                    if (s_screen == SC_ROOM_CONFIG) s_screen = SC_ROOM;
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
        case SC_JOIN_ROOM:   DrawTextScreen(u, a, b, "JOIN ROOM", s_codeBuf, "JOIN", true,
                                            OnCodeSubmit, "Enter the 4-letter room code"); break;
        case SC_ROOM:        DrawRoom(u, s, a, b); break;
        case SC_ROOM_CONFIG: DrawRoomConfig(u, s, a, b); break;
    }

    EndWin();
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
    IM_COL32(70, 140, 255, 255), IM_COL32(235, 235, 235, 255),
    IM_COL32(170, 240, 60, 255), IM_COL32(230, 70, 255, 255),
    IM_COL32(255, 95, 170, 255), IM_COL32(255, 220, 70, 255),
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
    float ds_[8]; ImU32 cs_[8]; bool dc_[8];
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
            }
    float myx = X(s.myPct);
    /* lead fill over [x(myPct), x(d)] for whoever is ahead; drawing
     * nearest-last makes each visible segment a lead over the next */
    bool anyLead = false;
    for (int i = 0; i < n; i++) {           /* farthest first */
        float d = ds_[i];
        if (d <= s.myPct + 0.05f) continue;
        anyLead = true;
        ImU32 c = cs_[i];
        int a = dc_[i] ? 80 : 235;
        ImVec2 p0(myx, 692 * k), p1(X(d), 710 * k);
        dl->AddRectFilledMultiColor(
            p0, p1, _shade(c, 1.35f, a), _shade(c, 1.35f, a),
            _shade(c, 1.0f, a), _shade(c, 1.0f, a));
        dl->AddLine(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y),
                    IM_COL32(255, 255, 255, dc_[i] ? 28 : 70), 1.0f);
        dl->AddRectFilled(ImVec2(p1.x - 2 * k, p0.y), p1,
                          _shade(c, 0.6f, a), 0);
    }
    /* dark seam where the local player's own fill ends */
    if (anyLead)
        dl->AddRectFilled(ImVec2(myx, 692 * k), ImVec2(myx + 2 * k, 710 * k),
                          IM_COL32(0, 0, 0, 170), 0);
    /* pointers for EVERY other player (ahead or behind) */
    for (int i = 0; i < n; i++) {
        float d = ds_[i];
        ImU32 c = cs_[i];
        int a = dc_[i] ? 102 : 255;
        ImVec2 tip(X(d), 690 * k);
        dl->AddTriangleFilled(
            ImVec2(tip.x - 5 * k, 682 * k), ImVec2(tip.x + 5 * k, 682 * k),
            tip, (c & 0x00ffffff) | ((ImU32)a << 24));
        dl->AddTriangle(
            ImVec2(tip.x - 5 * k, 682 * k), ImVec2(tip.x + 5 * k, 682 * k),
            tip, IM_COL32(0, 0, 0, dc_[i] ? 64 : 160), 1.0f);
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

void TriggerRankGlow(bool up) {
    s_glowUp = up ? 1 : 0;
    s_glowStart = _ms();
    s_glowLastAt = s_glowStart;
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
        /* module by cid */
        int32_t mod = -1;
        for (int m = 0; m < s.memberCount; m++)
            if (strcmp(s.members[m].cid, p.cid) == 0) { mod = s.members[m].module; break; }
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

void DrawLeaderboard() {
    Snapshot s;
    GetSnapshot(&s);
    if (s.boardCount == 0) return;
    float k = ImGui::GetIO().DisplaySize.y / 720.0f;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    /* top-right at (W - 8, 48) in 720p: right of/above the old spot and
     * below the note-rating icons (~x 1040-1260, y 15-40) */
    ImGui::SetNextWindowPos(ImVec2(ds.x - 8 * k, 48 * k), ImGuiCond_Always,
                            ImVec2(1, 0));
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
        if (!b.me) {
            ImU32 rc = RaceColor(s, b.cid);
            ImGui::PushID(r);
            ImGui::ColorButton("##rc", ImVec4(
                (rc & 0xff) / 255.f, ((rc >> 8) & 0xff) / 255.f,
                ((rc >> 16) & 0xff) / 255.f, 1.0f),
                ImGuiColorEditFlags_NoTooltip, ImVec2(8 * k, 8 * k));
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::TextColored(c, "%d %-10.10s %.1f%%", b.rank > 0 ? b.rank : r + 1,
                           b.name, (double)b.pct);
        ImVec4 lc = b.life >= 77 ? ImVec4(0.35f, 0.80f, 0.51f, 1)
                  : b.life >= 40 ? ImVec4(1.0f, 0.67f, 0.27f, 1)
                                 : ImVec4(0.90f, 0.31f, 0.27f, 1);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, lc);
        ImGui::PushStyleColor(ImGuiCol_FrameBg,
                              ImVec4(0.15f, 0.15f, 0.15f, 1));
        ImGui::ProgressBar(b.life / 255.0f, ImVec2(44 * k, 6 * k), "");
        ImGui::PopStyleColor(2);
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
