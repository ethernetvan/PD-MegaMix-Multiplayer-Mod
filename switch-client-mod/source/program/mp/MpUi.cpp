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
                float scale = 1.1f, bool enabled = true) const {
        bool hov = enabled && Hover(a, b);
        bool prs = hov && down;
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
            float pulse = 0.65f + 0.35f * sinf(t * 6.0f);
            ImU32 ring = (kAccent & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24);
            dl->AddRect(ImVec2(a.x - 2 * k, a.y - 2 * k), ImVec2(b.x + 2 * k, b.y + 2 * k),
                        ring, r + 2 * k, 0, 3.0f * k);
        } else if (st != B_GHOST) {
            dl->AddRect(a, b, border, r, 0, 1.2f);
        }
        TextCentered(a, b, label, scale, fg);
        return hov && clicked;
    }
    /* round X close button */
    bool CloseButton(ImVec2 c, float rad) const {
        bool hov = Hover(ImVec2(c.x - rad, c.y - rad), ImVec2(c.x + rad, c.y + rad));
        dl->AddCircleFilled(c, rad, hov && down ? kCardDown : (hov ? kCardHov : kCard), 32);
        dl->AddCircle(c, rad, hov ? kAccent : kLine, 32, hov ? 3.0f * k : 1.2f);
        float h = rad * 0.42f;
        dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), kText, 3.0f * k);
        dl->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), kText, 3.0f * k);
        return hov && clicked;
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
        bool hov = cards[i].enabled && u.Hover(ca, cb);
        u.dl->AddRectFilled(ca, cb, hov ? (u.down ? kCardDown : kCardHov) : kCard, 16 * u.k);
        if (hov) {
            float pulse = 0.65f + 0.35f * sinf(u.t * 6.0f);
            ImU32 ring = (kAccent & 0x00FFFFFF) | ((ImU32)(255 * pulse) << 24);
            u.dl->AddRect(ImVec2(ca.x - 2 * u.k, ca.y - 2 * u.k), ImVec2(cb.x + 2 * u.k, cb.y + 2 * u.k),
                          ring, 18 * u.k, 0, 3.5f * u.k);
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
        if (hov && u.clicked) {
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
    if (u.Button(ca, cb, "CANCEL", Ui::B_NORMAL, 1.55f)) {
        sound::PlayCancelSE();
        GoHomeOrRoom();
    }
    if (u.Button(oa, ob, okLabel, Ui::B_PRIMARY, 1.55f, buf[0] != 0)) {
        sound::PlayEnterSE();
        if (VirtualKeyboard::IsOpen()) VirtualKeyboard::Close();
        onOk();
    }
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
    if (u.Button(ea, eb, "Exit Room", Ui::B_DANGER, 1.4f)) {
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
        ImU32 fill = m.connected ? IM_COL32(230, 230, 230, 255) : IM_COL32(140, 140, 140, 255);
        bool you = strcmp(m.cid, s.myCid) == 0;
        /* avatar placeholder (silhouette sized to stay inside the disc) */
        u.dl->AddCircleFilled(ac, r, kCard, 48);
        u.dl->AddCircle(ac, r, you ? kAccent : kLine, 48, you ? 3.0f * u.k : 1.6f);
        IconPerson(u, ImVec2(ac.x, ac.y - r * 0.12f), r * 0.55f, fill, kInk);
        /* status dot */
        float dr = 9 * u.k;
        u.dl->AddCircleFilled(ImVec2(ac.x + r * 0.72f, ac.y + r * 0.72f), dr, MemberDot(m), 20);
        u.dl->AddCircle(ImVec2(ac.x + r * 0.72f, ac.y + r * 0.72f), dr, kBg, 20, 2.5f * u.k);
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
        bool rowHov = assignMode && !lead && u.Hover(ra, ImVec2(rb.x - 44 * u.k, rb.y));
        if (you || rowHov)
            u.dl->AddRectFilled(ra, rb, rowHov ? kCardHov : kAccentBg, 8 * u.k);
        char idx[6]; snprintf(idx, sizeof(idx), "%d.", i + 1);
        u.TextLeft(ImVec2(ra.x + 12 * u.k, ra.y), ImVec2(ra.x + 44 * u.k, rb.y), idx, 1.5f, kMuted);
        u.dl->AddCircleFilled(ImVec2(ra.x + 56 * u.k, (ra.y + rb.y) * 0.5f), 7 * u.k, MemberDot(m), 16);
        float nameR = rb.x - (owner && !you ? 64 * u.k : 8 * u.k);
        u.TextLeft(ImVec2(ra.x + 74 * u.k, ra.y), ImVec2(nameR, rb.y), m.name, 1.6f,
                   m.connected ? kText : kDim);
        if (lead) {
            /* leader tag right after the name */
            ImVec2 nsz = u.Size(m.name, 1.6f);
            float tx = ra.x + 74 * u.k + nsz.x + 12 * u.k;
            float tw = u.Size("LEADER", 0.95f).x + 16 * u.k;
            if (tx + tw + 4 * u.k < nameR) {
                float my = (ra.y + rb.y) * 0.5f;
                ImVec2 ta(tx, my - 11 * u.k), tb(tx + tw, my + 11 * u.k);
                u.dl->AddRectFilled(ta, tb, kGreenBg, 11 * u.k);
                u.dl->AddRect(ta, tb, kGreen, 11 * u.k, 0, 1.0f);
                u.TextCentered(ta, tb, "LEADER", 0.95f, kGreen);
            }
        }
        if (rowHov && u.clicked) {
            sound::PlaySelectSE();
            CmdPickMode("assigned", m.cid);
        }
        /* kick: owner only, never yourself */
        if (owner && !you) {
            float ks = 38 * u.k;
            ImVec2 ka(rb.x - 8 * u.k - ks, (ra.y + rb.y) * 0.5f - ks * 0.5f), kb(ka.x + ks, ka.y + ks);
            bool kh = u.Hover(ImVec2(ka.x - 6 * u.k, ka.y - 6 * u.k), ImVec2(kb.x + 6 * u.k, kb.y + 6 * u.k));
            u.dl->AddRectFilled(ka, kb, kh ? kRed : kRedBg, 5 * u.k);
            u.dl->AddRect(ka, kb, kRed, 5 * u.k, 0, kh ? 2.0f : 1.2f);
            float h = ks * 0.26f; ImVec2 c((ka.x + kb.x) * 0.5f, (ka.y + kb.y) * 0.5f);
            ImU32 xc = kh ? kInk : kRed;
            u.dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), xc, 2.5f * u.k);
            u.dl->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), xc, 2.5f * u.k);
            if (kh && u.clicked) {
                sound::PlayCancelSE();
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
    Footer(u, a, b, foot, reserve);
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
        bool hov = u.Hover(oa, ob);
        u.dl->AddRectFilled(oa, ob, sel ? kAccentBg : (hov ? kCardHov : kCard), 12 * u.k);
        u.dl->AddRect(oa, ob, (sel || hov) ? kAccent : kLine, 12 * u.k, 0, (sel || hov) ? 2.5f * u.k : 1.2f);
        /* radio */
        ImVec2 rc(oa.x + 24 * u.k, oa.y + 30 * u.k);
        u.dl->AddCircle(rc, 11 * u.k, sel ? kAccent : kMuted, 24, 2.0f * u.k);
        if (sel) u.dl->AddCircleFilled(rc, 6 * u.k, kAccent, 24);
        ImVec2 ls = u.Size(opts[i].label, 1.6f);
        u.Text(ImVec2(oa.x + 44 * u.k, rc.y - ls.y * 0.5f), opts[i].label, 1.6f, kText);
        char desc[96];
        u.Fit(opts[i].desc, ow - 28 * u.k, 1.15f, desc, sizeof(desc));
        u.Text(ImVec2(oa.x + 14 * u.k, oa.y + 62 * u.k), desc, 1.15f, kMuted);
        if (hov && u.clicked && !sel) {
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
        bool hov = assigned && u.Hover(ra, rb);
        u.dl->AddRectFilled(ra, rb, isPicker ? kAccentBg : (hov ? kCardHov : kCard), 8 * u.k);
        u.dl->AddRect(ra, rb, (isPicker || hov) ? kAccent : kLine, 8 * u.k, 0, 1.2f);
        u.dl->AddCircleFilled(ImVec2(ra.x + 18 * u.k, (ra.y + rb.y) * 0.5f), 6 * u.k, MemberDot(m), 16);
        char line[64];
        snprintf(line, sizeof(line), "%s%s", m.name, strcmp(m.cid, s.myCid) == 0 ? " (You)" : "");
        u.TextLeft(ImVec2(ra.x + 34 * u.k, ra.y), ImVec2(rb.x - 90 * u.k, rb.y), line, 1.4f,
                   m.connected ? kText : kDim);
        if (isPicker) {
            ImVec2 ps = u.Size("PICKER", 1.05f);
            u.Text(ImVec2(rb.x - 14 * u.k - ps.x, (ra.y + rb.y - ps.y) * 0.5f), "PICKER", 1.05f, kAccent);
        }
        if (hov && u.clicked && !isPicker) {
            sound::PlaySelectSE();
            CmdPickMode("assigned", m.cid);
        }
    }
    Footer(u, a, b, assigned ? "Tap a member to make them the song picker"
                             : "Pick ASSIGNED to choose a fixed song picker");
}

/* ---------------------------------------------------------------- main */
void DrawPanel() {
    bool open = OverlayOpen();
    if (open && !s_prevOpen) {
        /* (re)opening: land on the right screen */
        s_screen = InLobby() ? SC_ROOM : SC_HOME;
        s_kbArmed = false;
    }
    if (!open && s_prevOpen && VirtualKeyboard::IsOpen() && s_kbArmed)
        VirtualKeyboard::Close();
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
