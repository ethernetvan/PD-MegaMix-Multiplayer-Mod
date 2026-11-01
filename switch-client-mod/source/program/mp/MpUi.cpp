#include "MpLobby.hpp"
#include "MpAvatar.hpp"
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

static void HexPts(ImVec2 c, float r, ImVec2 out[6]) {
    for (int i = 0; i < 6; i++) {
        float ang = (-90.0f + 60.0f * i) * 3.14159265f / 180.0f;
        out[i] = ImVec2(c.x + cosf(ang) * r, c.y + sinf(ang) * r);
    }
}

/* filled hexagon, c0 at the top-left -> c1 at the bottom-right; with a
 * texture, the square [c-r, c+r] maps to uv0..uv1 (tint = vertex colour) */
static void HexFill(ImDrawList* dl, ImVec2 c, float r, ImU32 c0, ImU32 c1,
                    ImTextureID tex = nullptr, ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1)) {
    ImVec2 p[7];
    p[0] = c;
    HexPts(c, r, p + 1);
    if (tex) dl->PushTextureID(tex);
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(18, 7);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < 7; i++) {
        float t = ((p[i].x - c.x) + (p[i].y - c.y)) / (2.0f * 1.366f * r) + 0.5f;
        ImVec2 uv = tex ? ImVec2(uv0.x + (p[i].x - c.x + r) / (2.0f * r) * (uv1.x - uv0.x),
                                 uv0.y + (p[i].y - c.y + r) / (2.0f * r) * (uv1.y - uv0.y))
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

static void Footer(const Ui& u, ImVec2 a, ImVec2 b, const char* hint, float reserveR = 0.0f) {
    float fy = b.y - 44.0f * u.k;
    u.dl->AddLine(ImVec2(a.x + 24 * u.k, fy), ImVec2(b.x - 24 * u.k, fy), kLine, 1.5f);
    if (hint && *hint)
        u.TextLeft(ImVec2(a.x + 30 * u.k, fy), ImVec2(b.x - 30 * u.k - reserveR, b.y), hint, 1.3f, kMuted);
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

    /* three cards */
    float gap = 22 * u.k;
    float cy0 = top + 24 * u.k, cy1 = pa.y - 22 * u.k;
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
    bool owner = strcmp(s.ownerCid, s.myCid) == 0;

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
    if (s.voteKind[0])
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
    Footer(u, a, b, foot, reserve + gw + 10 * u.k);
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
    if (s_screen == SC_ROOM_CONFIG && strcmp(s.ownerCid, s.myCid) != 0) s_screen = SC_ROOM;
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
    /* above the overlay panel while it is open, otherwise below the top UI */
    float top = OverlayOpen() ? 6 * k : 60 * k;
    for (int i = 0; i < s.bannerCount; i++) {
        float ttl = s.banners[i].ttl;
        float alpha = ttl > 2.5f ? (3.0f - ttl) * 2.0f
                    : (ttl < 0.8f ? ttl / 0.8f : 1.0f);
        const char* txt = s.banners[i].text;
        ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, top + i * 34 * k),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
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
        ImGui::SetWindowFontScale(1.35f);
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
