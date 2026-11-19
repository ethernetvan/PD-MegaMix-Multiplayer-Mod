// PC panel for the MP lobby — desktop-style ImGui window restyled after
// Project DIVA Mega Mix's own UI (dark navy translucent body, slanted
// teal-gradient header, pill tabs, hexagon avatars, podium + member cards
// like the Switch room screen). Two tabs: Lobby and Settings.
// Toggle: F8 or `dbg_cmd overlay open|close`; Esc closes.
#include "MpLobby.hpp"
#include "MpAvatar.hpp"
#include "MpNet.hpp"
#include "../Config.hpp"
#include "../MpFont.hpp"
#include "../game/MpAutoPlay.hpp"
#include "../platform/MpLog.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace mp::lobby {

bool PadModeActive() { return false; }   /* PC is pointer/keyboard driven */
void RequestPadMode() {}

/* ---------------- game SEs (present thread -> game thread queue) ------- */
typedef void (__fastcall* SEFn)();
static SEFn SE_Enter  = (SEFn)0x1401A7F00;   /* PlayEnterSE  (nc diva.h)  */
static SEFn SE_Cancel = (SEFn)0x1401A7F20;   /* PlayCancelSE              */
static SEFn SE_Select = (SEFn)0x1401A7F60;   /* PlaySelectSE              */

static std::atomic<int> s_seQ[8];
static std::atomic<int> s_seW{0}, s_seR{0};

void PanelSE(int kind) {                /* 0 enter, 1 cancel, 2 select */
    int w = s_seW.load(std::memory_order_relaxed);
    int n = (w + 1) & 7;
    if (n == s_seR.load(std::memory_order_acquire)) return;
    s_seQ[w].store(kind, std::memory_order_relaxed);
    s_seW.store(n, std::memory_order_release);
}
void PumpPanelSE() {                    /* game thread, every input poll */
    for (;;) {
        int r = s_seR.load(std::memory_order_relaxed);
        if (r == s_seW.load(std::memory_order_acquire)) break;
        int k = s_seQ[r].load(std::memory_order_relaxed);
        s_seR.store((r + 1) & 7, std::memory_order_release);
        static const char* nm[] = { "enter", "cancel", "select" };
        WriteLog("[MP] panel SE %s\n", nm[k & 3]);
        if (k == 0) SE_Enter();
        else if (k == 1) SE_Cancel();
        else SE_Select();
    }
}

/* ---------------- DIVA palette ---------------- */
static const ImU32 kTeal      = IM_COL32(57, 197, 187, 255);   /* #39C5BB */
static const ImU32 kTealHi    = IM_COL32(96, 235, 222, 255);
static const ImU32 kTealDark  = IM_COL32(18, 96, 108, 255);
static const ImU32 kNavy      = IM_COL32(10, 22, 38, 242);
static const ImU32 kNavyCard  = IM_COL32(18, 34, 54, 232);
static const ImU32 kNavyHov   = IM_COL32(30, 54, 82, 240);
static const ImU32 kNavyField = IM_COL32(8, 16, 30, 245);
static const ImU32 kLineTeal  = IM_COL32(80, 210, 220, 70);
static const ImU32 kMuted     = IM_COL32(150, 162, 178, 255);
static const ImU32 kDim       = IM_COL32(110, 120, 135, 255);
static const ImU32 kText      = IM_COL32(240, 246, 252, 255);
static const ImU32 kGreen     = IM_COL32(90, 205, 130, 255);
static const ImU32 kRed       = IM_COL32(235, 85, 95, 255);
static const ImU32 kPink      = IM_COL32(255, 88, 150, 255);
static const ImU32 kYellow    = IM_COL32(255, 200, 80, 255);
static const ImU32 kGold      = IM_COL32(255, 214, 72, 255);
static const ImU32 kInk       = IM_COL32(12, 20, 30, 255);

/* sampled from the game's own menus (menu_select.png / results screen) */
static const ImU32 kHdrL      = IM_COL32(30, 64, 148, 255);   /* banner blue  */
static const ImU32 kHdrR      = IM_COL32(12, 172, 226, 255);  /* banner cyan  */
static const ImU32 kCyan      = IM_COL32(11, 202, 240, 255);  /* EASY pill    */
static const ImU32 kCardT     = IM_COL32(246, 249, 253, 242); /* light plate  */
static const ImU32 kCardB     = IM_COL32(212, 220, 232, 242);
static const ImU32 kCardYouT  = IM_COL32(255, 224, 92, 245);  /* selected bar */
static const ImU32 kCardYouB  = IM_COL32(224, 156, 8, 245);
static const ImU32 kCardGreyT = IM_COL32(216, 220, 226, 235); /* disconnected */
static const ImU32 kCardGreyB = IM_COL32(178, 184, 194, 235);
static const ImU32 kCardInk   = IM_COL32(22, 32, 54, 255);    /* navy text    */
static const ImU32 kCardDim   = IM_COL32(104, 112, 126, 255);
static const ImU32 kCardEdge  = IM_COL32(30, 42, 66, 200);
static const ImU32 kDotGreen  = IM_COL32(24, 158, 78, 255);
static const ImU32 kDotAmber  = IM_COL32(206, 138, 0, 255);
static const ImU32 kDotGrey   = IM_COL32(122, 130, 142, 255);

static float s_sc = 1.0f;    /* DisplaySize.y / 1080 clamped to >= 1 */
static inline float S(float v) { return v * s_sc; }

/* ---------------- small draw helpers ---------------- */
/* pointy-top hexagon */
static void HexPts(ImVec2 c, float r, ImVec2 out[6], float rotDeg = -90.0f) {
    for (int i = 0; i < 6; i++) {
        float a = (rotDeg + 60.0f * i) * 3.14159265f / 180.0f;
        out[i] = ImVec2(c.x + r * cosf(a), c.y + r * sinf(a));
    }
}
/* triangle-fan textured hex fill (same approach as the in-game draws) */
static void HexTex(ImDrawList* dl, ImVec2 c, float r, ImTextureID tex,
                   ImVec2 uv0, ImVec2 uv1, ImU32 tint = IM_COL32_WHITE) {
    ImVec2 p[7];
    p[0] = c;
    HexPts(c, r, p + 1);
    dl->PushTextureID(tex);
    dl->PrimReserve(18, 7);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < 7; i++) {
        ImVec2 uv(uv0.x + (p[i].x - c.x + r) / (2.0f * r) * (uv1.x - uv0.x),
                  uv0.y + (p[i].y - c.y + r) / (2.0f * r) * (uv1.y - uv0.y));
        dl->PrimWriteVtx(p[i], uv, tint);
    }
    for (int i = 0; i < 6; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % 6));
    }
    dl->PopTextureID();
}
static ImU32 Shade(ImU32 c, float f, int a) {
    int r = (int)((c & 0xff) * f), g = (int)(((c >> 8) & 0xff) * f),
        b = (int)(((c >> 16) & 0xff) * f);
    return IM_COL32(r > 255 ? 255 : r, g > 255 ? 255 : g,
                    b > 255 ? 255 : b, a);
}
/* hexagon filled with a top->bottom two-colour gradient */
static void HexFillV(ImDrawList* dl, ImVec2 c, float r, ImU32 cTop, ImU32 cBot,
                     ImTextureID tex = nullptr,
                     ImVec2 uv0 = ImVec2(0, 0), ImVec2 uv1 = ImVec2(1, 1)) {
    ImVec2 p[7];
    p[0] = c;
    HexPts(c, r, p + 1);
    if (tex) dl->PushTextureID(tex);
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(18, 7);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    for (int i = 0; i < 7; i++) {
        float t = (p[i].y - (c.y - r)) / (2.0f * r);
        ImU32 col;
        {   /* lerp cTop -> cBot */
            int dr = (int)((cTop & 0xff) + ((cBot & 0xff) - (int)(cTop & 0xff)) * t);
            int dg = (int)(((cTop >> 8) & 0xff) + ((int)((cBot >> 8) & 0xff) - (int)((cTop >> 8) & 0xff)) * t);
            int db = (int)(((cTop >> 16) & 0xff) + ((int)((cBot >> 16) & 0xff) - (int)((cTop >> 16) & 0xff)) * t);
            int da = (int)(((cTop >> 24) & 0xff) + ((int)((cBot >> 24) & 0xff) - (int)((cTop >> 24) & 0xff)) * t);
            col = IM_COL32(dr, dg, db, da);
        }
        ImVec2 uv = tex ? ImVec2(uv0.x + (p[i].x - c.x + r) / (2.0f * r) * (uv1.x - uv0.x),
                                 uv0.y + (p[i].y - c.y + r) / (2.0f * r) * (uv1.y - uv0.y))
                        : white;
        dl->PrimWriteVtx(p[i], uv, col);
    }
    for (int i = 0; i < 6; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % 6));
    }
    if (tex) dl->PopTextureID();
}

/* slanted parallelogram: both vertical edges lean `sk` px rightward at the
 * bottom (DIVA's signature slant); vertical gradient cTop -> cBot */
static void SlantGrad(ImDrawList* dl, ImVec2 a, ImVec2 b, float sk,
                      ImU32 cTop, ImU32 cBot) {
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(6, 4);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(ImVec2(a.x - sk, a.y), white, cTop);
    dl->PrimWriteVtx(ImVec2(b.x - sk, a.y), white, cTop);
    dl->PrimWriteVtx(ImVec2(b.x, b.y), white, cBot);
    dl->PrimWriteVtx(ImVec2(a.x, b.y), white, cBot);
    dl->PrimWriteIdx(base);     dl->PrimWriteIdx(base + 1);
    dl->PrimWriteIdx(base + 2);
    dl->PrimWriteIdx(base);     dl->PrimWriteIdx(base + 2);
    dl->PrimWriteIdx(base + 3);
}
/* left->right gradient variant (menu banner) */
static void SlantGradH(ImDrawList* dl, ImVec2 a, ImVec2 b, float sk,
                       ImU32 cL, ImU32 cR) {
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(6, 4);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(ImVec2(a.x - sk, a.y), white, cL);
    dl->PrimWriteVtx(ImVec2(b.x - sk, a.y), white, cR);
    dl->PrimWriteVtx(ImVec2(b.x, b.y), white, cR);
    dl->PrimWriteVtx(ImVec2(a.x, b.y), white, cL);
    dl->PrimWriteIdx(base);     dl->PrimWriteIdx(base + 1);
    dl->PrimWriteIdx(base + 2);
    dl->PrimWriteIdx(base);     dl->PrimWriteIdx(base + 2);
    dl->PrimWriteIdx(base + 3);
}
static void SlantLine(ImDrawList* dl, ImVec2 a, ImVec2 b, float sk,
                      ImU32 col, float th) {
    ImVec2 p[4] = { ImVec2(a.x - sk, a.y), ImVec2(b.x - sk, a.y),
                    ImVec2(b.x, b.y),      ImVec2(a.x, b.y) };
    dl->AddPolyline(p, 4, col, ImDrawFlags_Closed, th);
}

/* text helpers (ImGui font, scaled by s_sc) */
static void Txt(ImDrawList* dl, ImVec2 p, const char* s, float sc, ImU32 col) {
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * sc, p, col, s);
}
static void TxtC(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* s, float sc,
                 ImU32 col) {
    ImFont* f = ImGui::GetFont();
    float fs = ImGui::GetFontSize() * sc;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, s);
    dl->AddText(f, fs, ImVec2((a.x + b.x - ts.x) * 0.5f,
                              (a.y + b.y - ts.y) * 0.5f), col, s);
}
static float TxtW(const char* s, float sc) {
    return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * sc,
                                           FLT_MAX, 0.0f, s).x;
}
static void TxtBig(ImDrawList* dl, ImVec2 p, const char* s, float px, ImU32 col) {
    ImFont* f = FontLoader::Big();
    if (!f) { Txt(dl, p, s, px / ImGui::GetFontSize(), col); return; }
    dl->AddText(f, px, p, col, s);
}
static float TxtBigW(const char* s, float px) {
    ImFont* f = FontLoader::Big();
    if (!f) return TxtW(s, px / ImGui::GetFontSize());
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}
/* dark outline + fill, readable over busy menu art */
static void TxtShadow(ImDrawList* dl, ImVec2 p, const char* s, float sc,
                      ImU32 col) {
    float fs = ImGui::GetFontSize() * sc;
    ImFont* f = ImGui::GetFont();
    const ImU32 out = IM_COL32(6, 10, 16, 200);
    dl->AddText(f, fs, ImVec2(p.x - 1, p.y), out, s);
    dl->AddText(f, fs, ImVec2(p.x + 1, p.y), out, s);
    dl->AddText(f, fs, ImVec2(p.x, p.y - 1), out, s);
    dl->AddText(f, fs, ImVec2(p.x, p.y + 1), out, s);
    dl->AddText(f, fs, p, col, s);
}

/* ---------------- members data (Switch ports) ---------------- */
static const ImU32 kRacePalette[8] = {
    IM_COL32(165, 110, 255, 255), IM_COL32(255, 150, 40, 255),
    IM_COL32(70, 140, 255, 255),  IM_COL32(255, 75, 145, 255),
    IM_COL32(170, 240, 60, 255),  IM_COL32(230, 70, 255, 255),
    IM_COL32(45, 225, 165, 255),  IM_COL32(255, 220, 70, 255),
};
static ImU32 RaceColor(const Snapshot& s, const char* cid) {
    for (int i = 0; i < s.memberCount && i < 8; i++)
        if (strcmp(s.members[i].cid, cid) == 0) return kRacePalette[i];
    return IM_COL32(180, 180, 180, 255);
}
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
/* darker status colours that read on the light member plates */
static ImU32 MemberDotCard(const MemberInfo& m) {
    return !m.connected ? kDotGrey : (m.ready ? kDotGreen : kDotAmber);
}
/* session ranking order: srPos, join order for unknowns (stable sort) */
static int SessionOrder(const Snapshot& s, int* order) {
    int n = s.memberCount;
    for (int i = 0; i < n; i++) order[i] = i;
    auto key = [&](int i) { return s.members[i].srPos > 0 ? s.members[i].srPos : 99; };
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && key(order[j]) < key(order[j - 1]); j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    return n;
}
static mp::avatar::Ref LocalRef(int32_t local) {
    mp::avatar::Ref r;
    r.module = local;
    return r;
}
static mp::avatar::Ref MemberRef(const Snapshot& s, const MemberInfo& m) {
    int32_t local = mp::avatar::LocalModule();
    return (local >= 0 && strcmp(m.cid, s.myCid) == 0) ? LocalRef(local) : m.av;
}
static const char* RoleCaption(const Snapshot& s, const MemberInfo& m,
                               char* out, int n) {
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

/* flat grey trophy (Switch style): cup + handles + stem + base, place
 * number inside; h tall, centred on c.x, top at c.y */
static void DrawTrophy(ImDrawList* dl, ImVec2 c, float h, int place) {
    const ImU32 col = IM_COL32(150, 160, 175, 255);
    float x = c.x, t = c.y, w = h * 0.40f;
    float cupB = t + h * 0.74f;
    for (int side = -1; side <= 1; side += 2) {
        ImVec2 hc(x + side * w, t + h * 0.31f);
        float rx = h * 0.15f, ry = h * 0.20f;
        for (int k = 0; k <= 12; k++) {
            float an = -1.5707963f + 3.1415926f * (float)k / 12.0f;
            dl->PathLineTo(ImVec2(hc.x + side * rx * cosf(an),
                                  hc.y + ry * sinf(an)));
        }
        dl->PathStroke(col, 0, h * 0.08f);
    }
    dl->AddRectFilled(ImVec2(x - w, t), ImVec2(x + w, cupB), col,
                      w * 0.85f, ImDrawFlags_RoundCornersBottom);
    dl->AddRectFilled(ImVec2(x - h * 0.07f, cupB - h * 0.02f),
                      ImVec2(x + h * 0.07f, t + h * 0.88f), col);
    dl->AddRectFilled(ImVec2(x - h * 0.26f, t + h * 0.88f),
                      ImVec2(x + h * 0.26f, t + h), col, h * 0.05f);
    char d[2] = { (char)('0' + place), 0 };
    ImFont* f = ImGui::GetFont();
    float fs = h * 0.62f;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, d);
    dl->AddText(f, fs, ImVec2(x - ts.x * 0.5f,
                t + (cupB - t) * 0.5f - ts.y * 0.5f - h * 0.02f), kInk, d);
}
/* [1] n  [2] n  [3] n; vertically centred on a.y, left edge at a.x.
 * draw=false only measures; returns the width. */
static float DrawTrophyRow(ImDrawList* dl, ImVec2 a, float h,
                           const int32_t places[3], bool draw = true,
                           float gap = 12.0f, ImU32 nCol = kText,
                           float nsc = 0.0f) {
    float x = a.x;
    float iw = h * 1.00f;
    if (nsc <= 0.0f) nsc = h * 0.70f / ImGui::GetFontSize();
    for (int i = 0; i < 3; i++) {
        char n[8]; snprintf(n, sizeof(n), "%d", places[i]);
        float nw = TxtW(n, nsc);
        if (draw) {
            DrawTrophy(dl, ImVec2(x + iw * 0.5f, a.y - h * 0.5f), h, i + 1);
            Txt(dl, ImVec2(x + iw + 4 * s_sc,
                           a.y - ImGui::GetFontSize() * nsc * 0.5f),
                n, nsc, places[i] ? nCol : kDim);
        }
        x += iw + 3 * s_sc + nw + (i < 2 ? gap * s_sc : 0);
    }
    return x - a.x;
}

/* hexagon avatar framed in the player's race colour (gradient frame,
 * white ring, pale plate, portrait or silhouette) - port of the in-song
 * DrawRaceAvatar styling */
static void DrawHexAvatar(ImDrawList* dl, ImVec2 c, float r,
                          const mp::avatar::Ref& av, ImU32 raceCol,
                          bool connected, bool you = false) {
    int a = connected ? 255 : 130;
    HexFillV(dl, ImVec2(c.x, c.y + S(2.0f)), r + S(1.0f),
             IM_COL32(0, 0, 0, connected ? 115 : 55),
             IM_COL32(0, 0, 0, connected ? 115 : 55));
    ImU32 f0 = connected ? Shade(raceCol, 1.22f, 255)
                         : IM_COL32(150, 150, 150, a);
    ImU32 f1 = connected ? Shade(raceCol, 0.68f, 255)
                         : IM_COL32(105, 105, 105, a);
    HexFillV(dl, c, r, f0, f1);
    ImVec2 p[6];
    HexPts(c, r - 1.2f * s_sc, p);
    ImVec2 bevel[3] = { p[4], p[5], p[0] };
    dl->AddPolyline(bevel, 3, IM_COL32(255, 255, 255, connected ? 140 : 60),
                    0, 1.6f * s_sc);
    float ri = r * 0.81f;
    HexFillV(dl, c, ri + 1.4f * s_sc,
             IM_COL32(255, 255, 255, connected ? 245 : 130),
             IM_COL32(255, 255, 255, connected ? 245 : 130));
    HexFillV(dl, c, ri,
             connected ? IM_COL32(255, 232, 242, 255)
                       : IM_COL32(200, 200, 200, a),
             connected ? IM_COL32(255, 220, 198, 255)
                       : IM_COL32(170, 170, 170, a));
    void* tex = nullptr;
    float uv0[2], uv1[2];
    if (mp::avatar::Resolve(av, &tex, uv0, uv1) == 1 && tex) {
        ImU32 tint = connected ? IM_COL32_WHITE : IM_COL32(145, 145, 145, a);
        HexFillV(dl, c, ri, tint, tint, (ImTextureID)tex,
                 ImVec2(uv0[0], uv0[1]), ImVec2(uv1[0], uv1[1]));
    } else {
        /* silhouette: head + shoulders */
        ImU32 sil = connected ? Shade(raceCol, 0.95f, 240)
                              : IM_COL32(140, 140, 140, a);
        dl->AddCircleFilled(ImVec2(c.x, c.y - ri * 0.28f), ri * 0.30f, sil, 24);
        ImVec2 q[4] = {
            ImVec2(c.x - ri * 0.44f, c.y + ri * 0.52f),
            ImVec2(c.x - ri * 0.34f, c.y + ri * 0.10f),
            ImVec2(c.x + ri * 0.34f, c.y + ri * 0.10f),
            ImVec2(c.x + ri * 0.44f, c.y + ri * 0.52f),
        };
        dl->AddConvexPolyFilled(q, 4, sil);
    }
    if (you) {
        HexPts(c, r + 2.5f * s_sc, p);
        dl->AddPolyline(p, 6, kTealHi, ImDrawFlags_Closed, 1.6f * s_sc);
    }
}
static void StatusDot(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircleFilled(c, r, col, 16);
    dl->AddCircle(c, r, IM_COL32(8, 14, 24, 255), 16, 1.6f * s_sc);
}

/* slanted role tag; returns its width */
static float Tag(ImDrawList* dl, ImVec2 p, float h, const char* s,
                 ImU32 fg, ImU32 bg) {
    float w = TxtW(s, 0.78f) + h * 0.9f;
    float sk = h * 0.35f;
    ImU32 b0 = (bg & 0x00FFFFFF) | 210u << 24;
    SlantGrad(dl, p, ImVec2(p.x + w, p.y + h), sk, b0,
              (bg & 0x00FFFFFF) | 160u << 24);
    TxtC(dl, ImVec2(p.x - sk, p.y), ImVec2(p.x + w, p.y + h), s, 0.78f, fg);
    return w;
}

/* ---------------- DIVA widgets ---------------- */
/* slanted teal button; returns click. id must be unique. */
enum BtnKind { BT_TEAL, BT_DANGER, BT_DARK, BT_GHOST, BT_YELLOW, BT_GREY };
static bool DivaBtn(ImVec2 a, ImVec2 b, const char* label, BtnKind kind,
                    bool enabled = true, float tsc = 0.95f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = b.y - a.y, sk = h * 0.35f;
    ImGui::SetCursorScreenPos(ImVec2(a.x - sk, a.y));
    char id[96];
    snprintf(id, sizeof(id), "##db%s", label);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(b.x - a.x + sk, h));
    bool hov = enabled && ImGui::IsItemHovered();
    bool dn = hov && ImGui::IsMouseDown(0);
    ImU32 c0, c1, tcol = IM_COL32_WHITE;
    switch (kind) {
    case BT_DANGER:
        c0 = dn ? IM_COL32(168, 30, 62, 255) : hov ? IM_COL32(255, 122, 160, 255) : IM_COL32(240, 82, 122, 255);
        c1 = dn ? IM_COL32(124, 18, 46, 255) : hov ? IM_COL32(224, 66, 106, 255) : IM_COL32(198, 46, 82, 255);
        break;
    case BT_DARK:
        c0 = dn ? IM_COL32(18, 32, 48, 255) : hov ? IM_COL32(42, 68, 100, 255) : IM_COL32(26, 44, 66, 240);
        c1 = dn ? IM_COL32(12, 22, 34, 255) : hov ? IM_COL32(28, 46, 70, 255) : IM_COL32(16, 30, 46, 240);
        break;
    case BT_GHOST:
        c0 = dn ? IM_COL32(20, 30, 44, 230) : hov ? IM_COL32(40, 60, 86, 230) : IM_COL32(24, 36, 54, 200);
        c1 = dn ? IM_COL32(14, 22, 32, 230) : hov ? IM_COL32(28, 42, 62, 230) : IM_COL32(16, 26, 40, 200);
        break;
    case BT_YELLOW:
        c0 = dn ? IM_COL32(196, 146, 0, 255) : hov ? IM_COL32(255, 242, 84, 255) : IM_COL32(255, 228, 10, 255);
        c1 = dn ? IM_COL32(164, 120, 0, 255) : hov ? IM_COL32(240, 192, 22, 255) : IM_COL32(219, 168, 0, 255);
        tcol = kCardInk;
        break;
    case BT_GREY:
        c0 = dn ? IM_COL32(122, 130, 142, 255) : hov ? IM_COL32(218, 225, 234, 255) : IM_COL32(196, 204, 214, 235);
        c1 = dn ? IM_COL32(98, 106, 118, 255) : hov ? IM_COL32(178, 186, 198, 255) : IM_COL32(152, 160, 170, 235);
        tcol = kCardInk;
        break;
    default: /* cyan pill (EASY tab) */
        c0 = dn ? IM_COL32(0, 116, 158, 255) : hov ? IM_COL32(88, 228, 255, 255) : IM_COL32(28, 200, 240, 255);
        c1 = dn ? IM_COL32(0, 92, 130, 255) : hov ? IM_COL32(16, 178, 224, 255) : IM_COL32(4, 142, 192, 255);
    }
    if (!enabled) {
        c0 = IM_COL32(140, 148, 158, 200); c1 = IM_COL32(110, 118, 128, 200);
        tcol = IM_COL32(70, 78, 88, 255);
    }
    SlantGrad(dl, a, b, sk, c0, c1);
    ImU32 ol = !enabled ? IM_COL32(70, 80, 95, 120)
               : kind == BT_YELLOW || kind == BT_GREY
                     ? IM_COL32(88, 96, 108, 220)
               : hov ? kTealHi : kLineTeal;
    SlantLine(dl, a, b, sk, ol, hov ? 2.0f : 1.2f);
    TxtC(dl, ImVec2(a.x - sk, a.y), b, label, tsc,
         enabled ? tcol : IM_COL32(80, 88, 98, 255));
    return enabled && pressed;
}

/* styled InputText: dark navy field + teal border (brighter when active);
 * the chrome is drawn first so the widget's own text lands on top */
static bool DivaInput(ImVec2 a, float w, const char* id, char* buf, int n,
                      ImGuiInputTextFlags flags = 0,
                      const char* hint = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFontSize() + S(12);
    ImVec2 p0(a.x, a.y), p1(a.x + w, a.y + h);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 16, 30, 235), S(4));
    ImGui::SetCursorScreenPos(ImVec2(a.x + S(2), a.y + S(1)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(w - S(4));
    bool ret = hint ? ImGui::InputTextWithHint(id, hint, buf, n, flags)
                    : ImGui::InputText(id, buf, n, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    bool act = ImGui::IsItemActive();
    dl->AddRect(p0, p1, act ? kTeal : IM_COL32(60, 90, 110, 160), S(4), 0,
                act ? 1.8f : 1.0f);
    return ret;
}

/* section header: small teal bar + label */
static void SectionHdr(ImDrawList* dl, ImVec2 p, const char* s) {
    dl->AddRectFilled(p, ImVec2(p.x + S(4), p.y + S(18)), kTeal, S(2));
    Txt(dl, ImVec2(p.x + S(12), p.y + S(1)), s, 0.95f, kTealHi);
}

/* ---------------- Lobby tab ---------------- */
static char s_nameBuf[40] = "";
static bool s_nameInit = false;
static char s_codeBuf[8] = "";
static char s_urlBuf[160] = "";
static bool s_urlInit = false;
static char s_kickCid[20] = "";
static bool s_confirmLeave = false;
static int  s_tab = 0;          /* 0 lobby, 1 settings */

/* podium: session top-3 (Switch DrawRoom port); returns the bottom y */
static float DrawPodium(ImDrawList* dl, const Snapshot& sn, const int* order,
                        ImVec2 a, ImVec2 b) {
    int n = sn.memberCount < 3 ? sn.memberCount : 3;
    float cw = (b.x - a.x) / 3.0f;
    float xoff = (3.0f - n) * cw * 0.5f;
    const float rads[3] = { S(33), S(27), S(24) };
    float cy = a.y + S(40);
    float bot = a.y;
    for (int i = 0; i < n; i++) {
        const MemberInfo& m = sn.members[order[i]];
        float cx = a.x + xoff + cw * (i + 0.5f);
        float r = rads[i];
        ImVec2 ac(cx, cy);
        bool you = strcmp(m.cid, sn.myCid) == 0;
        DrawHexAvatar(dl, ac, r, MemberRef(sn, m), RaceColor(sn, m.cid),
                      m.connected, you);
        StatusDot(dl, ImVec2(ac.x + r * 0.66f, ac.y + r * 0.66f), S(6.5f),
                  MemberDot(m));
        /* numbered white plate overlapping the hexagon's bottom tip */
        float pw = S(30), ph = S(24);
        ImVec2 pa(cx - pw * 0.5f, cy + r * 0.58f), pb(pa.x + pw, pa.y + ph);
        dl->AddRectFilled(pa, pb, IM_COL32(248, 248, 250, 255), S(3));
        dl->AddRect(pa, pb, kCardInk, S(3), 0, 1.4f);
        char num[4]; snprintf(num, sizeof(num), "%d", i + 1);
        TxtC(dl, pa, pb, num, i == 0 ? 1.15f : 0.95f, kCardInk);
        /* name + role caption + trophies stacked below the plate */
        const char* nm = m.name[0] ? m.name : "(unnamed)";
        float ny = pb.y + S(4);
        TxtShadow(dl, ImVec2(cx - TxtW(nm, 1.0f) * 0.5f, ny), nm, 1.0f,
                  m.connected ? kText : kDim);
        ny += ImGui::GetFontSize() + S(2);
        char cap[48];
        RoleCaption(sn, m, cap, sizeof(cap));
        if (cap[0]) {
            Txt(dl, ImVec2(cx - TxtW(cap, 0.8f) * 0.5f, ny), cap, 0.8f, kMuted);
            ny += ImGui::GetFontSize() * 0.8f + S(2);
        }
        float th = S(14);
        float tw = DrawTrophyRow(dl, ImVec2(0, 0), th, m.places, false, 8.0f);
        DrawTrophyRow(dl, ImVec2(cx - tw * 0.5f, ny + th * 0.5f + S(1)), th,
                      m.places);
        float bb = ny + th + S(4);
        if (bb > bot) bot = bb;
    }
    return bot;
}

/* one member card: [#] [hex avatar] [name+tags] [status] [trophies]
 * [wins] [Kick] on a light plate (menu_select unselected song bar) */
static bool MemberRow(ImDrawList* dl, const Snapshot& sn, const MemberInfo& m,
                      int idx, ImVec2 ra, ImVec2 rb, bool owner, bool assign) {
    bool you = strcmp(m.cid, sn.myCid) == 0;
    bool hov = false, clk = false;
    if (assign) {
        float rowR = rb.x;
        if (owner && !you)           /* leave the Kick button clickable */
            rowR -= S(10) + S(52) + S(26) * 0.35f;
        ImGui::SetCursorScreenPos(ra);
        char id[32]; snprintf(id, sizeof(id), "##row%d", idx);
        ImGui::InvisibleButton(id, ImVec2(rowR - ra.x, rb.y - ra.y));
        hov = ImGui::IsItemHovered();
        clk = ImGui::IsItemClicked(0);
    }
    float sk = S(10);
    ImU32 cT = !m.connected ? kCardGreyT : you ? kCardYouT
               : hov ? IM_COL32(255, 255, 255, 248) : kCardT;
    ImU32 cB = !m.connected ? kCardGreyB : you ? kCardYouB
               : hov ? IM_COL32(226, 234, 244, 248) : kCardB;
    SlantGrad(dl, ra, rb, sk, cT, cB);
    SlantLine(dl, ra, rb, sk, hov ? kCyan : kCardEdge, hov ? 1.8f : 1.2f);
    float my = (ra.y + rb.y) * 0.5f;
    const ImU32 ink = kCardInk, dim = kCardDim;
    /* # */
    char num[8]; snprintf(num, sizeof(num), "%d", idx + 1);
    Txt(dl, ImVec2(ra.x + S(10), my - ImGui::GetFontSize() * 0.5f), num,
        1.0f, dim);
    /* hex avatar + status dot */
    float hr = (rb.y - ra.y) * 0.38f;
    ImVec2 hc(ra.x + S(42), my);
    DrawHexAvatar(dl, hc, hr, MemberRef(sn, m), RaceColor(sn, m.cid),
                  m.connected, false);
    StatusDot(dl, ImVec2(hc.x + hr * 0.68f, hc.y + hr * 0.68f), S(4.5f),
              MemberDotCard(m));
    /* right-side columns, anchored from the right edge:
     * [status] [trophies 1/2/3] [wins] [kick] */
    float xr = rb.x - S(10);
    if (owner && !you) {
        float bw = S(52), bh = S(26);
        ImVec2 ka(xr - bw, my - bh * 0.5f), kb(ka.x + bw, ka.y + bh);
        float sk2 = bh * 0.35f;
        ImGui::SetCursorScreenPos(ImVec2(ka.x - sk2, ka.y));
        char kid[32]; snprintf(kid, sizeof(kid), "##kick%d", idx);
        bool kc = ImGui::InvisibleButton(kid, ImVec2(bw + sk2, bh));
        bool kh = ImGui::IsItemHovered();
        SlantGrad(dl, ka, kb, sk2,
                  kh ? IM_COL32(255, 130, 165, 255) : IM_COL32(238, 84, 122, 255),
                  kh ? IM_COL32(215, 70, 105, 255) : IM_COL32(196, 42, 78, 255));
        SlantLine(dl, ka, kb, sk2, IM_COL32(120, 20, 44, 220),
                  kh ? 2.0f : 1.2f);
        TxtC(dl, ImVec2(ka.x - sk2, ka.y), kb, "Kick", 0.9f, IM_COL32_WHITE);
        xr = ka.x - sk2 - S(10);
        if (kc) {
            strncpy(s_kickCid, m.cid, sizeof(s_kickCid) - 1);
            ImGui::OpenPopup("Kick?");
            PanelSE(2);
        }
    }
    /* wins */
    char wb[16];
    snprintf(wb, sizeof(wb), "%dW", m.wins);
    float ww = TxtW(wb, 0.95f);
    Txt(dl, ImVec2(xr - ww, my - ImGui::GetFontSize() * 0.475f), wb, 0.95f,
        m.wins > 0 ? IM_COL32(178, 122, 0, 255) : dim);
    xr -= S(52);
    /* trophies 1/2/3 with full-size counts */
    float thh = S(15);
    float tw = DrawTrophyRow(dl, ImVec2(0, 0), thh, m.places, false, 9.0f,
                             ink, 1.0f);
    DrawTrophyRow(dl, ImVec2(xr - tw, my), thh, m.places, true, 9.0f,
                  ink, 1.0f);
    xr -= tw + S(12);
    /* status, right-aligned inside its column */
    const char* ms = MemberStatus(m);
    ImU32 dc = MemberDotCard(m);
    float sw = TxtW(ms, 1.0f);
    StatusDot(dl, ImVec2(xr - sw - S(11), my), S(4.5f), dc);
    Txt(dl, ImVec2(xr - sw, my - ImGui::GetFontSize() * 0.5f), ms, 1.0f, dc);
    float stLeft = xr - S(140);
    /* name + tags in the remaining zone, clipped */
    float nx = hc.x + hr + S(10);
    dl->PushClipRect(ImVec2(nx - S(4), ra.y), ImVec2(stLeft - S(6), rb.y),
                     true);
    const char* nm = m.name[0] ? m.name : "(unnamed)";
    Txt(dl, ImVec2(nx, my - ImGui::GetFontSize() * 0.575f), nm, 1.15f,
        m.connected ? ink : dim);
    nx += TxtW(nm, 1.15f) + S(6);
    float th = S(16), tyy = my - th * 0.5f;
    struct { const char* s; ImU32 fg; ImU32 bg; } tags[5];
    int nt = 0;
    if (you)                                tags[nt++] = { "YOU",    kCardInk, kCyan };
    if (strcmp(m.cid, sn.ownerCid) == 0)    tags[nt++] = { "OWNER",  kCardInk, kGold };
    if (strcmp(m.cid, sn.leaderCid) == 0)   tags[nt++] = { "LEADER", kCardInk, kGreen };
    if (m.autoplay)                         tags[nt++] = { "AUTO",   IM_COL32_WHITE, kPink };
    if (sn.assignedCid[0] && strcmp(sn.assignedCid, "null") != 0 &&
        strcmp(m.cid, sn.assignedCid) == 0 && strcmp(m.cid, sn.leaderCid) != 0)
        tags[nt++] = { "PICKER", kCardInk, kYellow };
    for (int i = 0; i < nt; i++)
        nx += Tag(dl, ImVec2(nx, tyy), th, tags[i].s, tags[i].fg,
                  tags[i].bg) + S(4);
    dl->PopClipRect();
    return clk;
}

static float LobbyTab(const Snapshot& sn, ImVec2 a, ImVec2 b) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = a.y;

    /* --- player name (top of the left column) --- */
    if (!s_nameInit) {
        strncpy(s_nameBuf, Config::mpPlayerName.c_str(), sizeof(s_nameBuf) - 1);
        s_nameInit = true;
    }
    Txt(dl, ImVec2(a.x, y + S(5)), "PLAYER NAME", 0.85f, kMuted);
    bool submit = DivaInput(ImVec2(a.x + S(110), y), S(170), "##pname",
                            s_nameBuf, sizeof(s_nameBuf),
                            ImGuiInputTextFlags_EnterReturnsTrue);
    if (DivaBtn(ImVec2(a.x + S(290), y), ImVec2(a.x + S(344), y + S(26)),
                "Save", BT_DARK, true, 0.8f) || submit) {
        if (s_nameBuf[0] && strncmp(s_nameBuf, Config::mpPlayerName.c_str(),
                                    sizeof(s_nameBuf)) != 0) {
            PanelSE(0);
            CmdSetName(s_nameBuf);
        }
    }
    y += S(38);

    if (!sn.code[0]) {
        /* --- not in a room --- */
        SectionHdr(dl, ImVec2(a.x, y), "ROOM");
        y += S(30);
        if (DivaBtn(ImVec2(a.x, y), ImVec2(a.x + S(180), y + S(40)),
                    "CREATE ROOM", BT_TEAL, true, 1.0f)) {
            PanelSE(0);
            CmdLobbyCreate();
        }
        bool codeEnter = DivaInput(ImVec2(a.x + S(196), y + S(5)), S(120),
                  "##code", s_codeBuf, sizeof(s_codeBuf),
                  ImGuiInputTextFlags_CharsUppercase |
                  ImGuiInputTextFlags_EnterReturnsTrue, "CODE");
        if (DivaBtn(ImVec2(a.x + S(330), y), ImVec2(a.x + S(400), y + S(40)),
                    "JOIN", BT_DARK, s_codeBuf[0] != 0, 1.0f) ||
            (codeEnter && s_codeBuf[0])) {
            if (s_codeBuf[0]) {
                PanelSE(0);
                CmdLobbyJoin(s_codeBuf);
            }
        }
        y += S(56);
        Txt(dl, ImVec2(a.x, y), "Create a room or join one with a code.",
            0.9f, kDim);
        y += S(24);
        /* connection status */
        MpStatus mst;
        mp::GetStatus(&mst);
        char st[160];
        if (mst.state == MpState::Connected)
            snprintf(st, sizeof(st), "%s  %s  %d ms",
                     mp::StateName(mst.state), mst.server, mst.rtt_ms);
        else if (mst.last_error[0])
            snprintf(st, sizeof(st), "%s  %s  (%s)",
                     mp::StateName(mst.state), mst.server, mst.last_error);
        else
            snprintf(st, sizeof(st), "%s  %s", mp::StateName(mst.state),
                     mst.server);
        ImU32 stc = mst.state == MpState::Connected ? kGreen :
                    mst.state == MpState::Error ? kRed : kMuted;
        StatusDot(dl, ImVec2(a.x + S(6), y + S(7)), S(4), stc);
        Txt(dl, ImVec2(a.x + S(16), y), st, 0.9f, stc);
        return y + S(18);
    }

    /* --- in a room: left = podium + room settings, right = member cards --- */
    bool owner = strcmp(sn.myCid, sn.ownerCid) == 0 && sn.connected;
    int order[8];
    SessionOrder(sn, order);
    float splitX = a.x + S(360);
    float lx = a.x, rx = splitX + S(16);

    /* LEFT: podium directly under the name row */
    float ly = y;
    if (sn.memberCount >= 2)
        ly = DrawPodium(dl, sn, order, ImVec2(lx, ly),
                        ImVec2(splitX, ly + S(148))) + S(8);
    /* LEFT: room settings */
    SectionHdr(dl, ImVec2(lx, ly), "ROOM");
    ly += S(28);
    const char* modes[] = { "rotate", "owner", "assigned" };
    int mi = strncmp(sn.pickMode, "assigned", 8) == 0 ? 2 :
             strncmp(sn.pickMode, "owner", 5) == 0 ? 1 : 0;
    if (owner) {
        const float ctlX = lx + S(104), ctlW = S(190);
        Txt(dl, ImVec2(lx, ly + S(4)), "Pick mode", 0.9f, kText);
        ImGui::SetCursorScreenPos(ImVec2(ctlX, ly));
        ImGui::SetNextItemWidth(ctlW);
        if (ImGui::Combo("##pickmode", &mi, modes, 3)) {
            PanelSE(2);
            if (mi == 2) {
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
        ly += S(36);
        Txt(dl, ImVec2(lx, ly + S(4)), "Assignee", 0.9f, kText);
        ImGui::SetCursorScreenPos(ImVec2(ctlX, ly));
        ImGui::SetNextItemWidth(ctlW);
        const char* assignedName = "(none)";
        if (sn.assignedCid[0] && strcmp(sn.assignedCid, "null") != 0)
            for (int i = 0; i < sn.memberCount; i++)
                if (strcmp(sn.members[i].cid, sn.assignedCid) == 0)
                    assignedName = sn.members[i].name;
        ImGui::BeginDisabled(mi != 2);
        if (ImGui::BeginCombo("##assign", assignedName)) {
            for (int i = 0; i < sn.memberCount; i++) {
                if (ImGui::Selectable(sn.members[i].name)) {
                    PanelSE(2);
                    CmdPickMode("assigned", sn.members[i].cid);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ly += S(36);
        if (mi == 2) {
            Txt(dl, ImVec2(ctlX, ly),
                "Click a member to make them the song picker", 0.8f, kDim);
            ly += S(22);
        }
    } else {
        Txt(dl, ImVec2(lx, ly + S(4)), "Pick mode", 0.9f, kText);
        char pm[32];
        snprintf(pm, sizeof(pm), "%c%s",
                 sn.pickMode[0] >= 'a' && sn.pickMode[0] <= 'z'
                     ? sn.pickMode[0] - 32 : sn.pickMode[0],
                 sn.pickMode + 1);
        Txt(dl, ImVec2(lx + S(104), ly + S(4)), pm, 0.9f, kMuted);
        ly += S(36);
        if (mi == 2) {
            const char* who = "(none)";
            if (sn.assignedCid[0] && strcmp(sn.assignedCid, "null") != 0)
                for (int i = 0; i < sn.memberCount; i++)
                    if (strcmp(sn.members[i].cid, sn.assignedCid) == 0)
                        who = sn.members[i].name;
            Txt(dl, ImVec2(lx, ly + S(4)), "Assignee", 0.9f, kText);
            Txt(dl, ImVec2(lx + S(104), ly + S(4)), who, 0.9f, kMuted);
            ly += S(36);
        }
    }
    if (sn.selPv > 0) {
        char sl[128];
        snprintf(sl, sizeof(sl), "Song  pv %d  %s", sn.selPv, sn.selName);
        Txt(dl, ImVec2(lx, ly), sl, 0.85f, kMuted);
        ly += S(24);
    }

    /* RIGHT: member cards */
    bool assignMode = owner && strcmp(sn.pickMode, "assigned") == 0;
    float ry = a.y;
    for (int i = 0; i < sn.memberCount; i++) {
        const MemberInfo& m = sn.members[order[i]];
        ImVec2 ra(rx, ry), rb(b.x, ry + S(48));
        if (MemberRow(dl, sn, m, i, ra, rb, owner,
                      assignMode && !m.is_leader)) {
            PanelSE(2);
            CmdPickMode("assigned", m.cid);
        }
        ry += S(56);
    }
    /* kick confirmation modal */
    if (ImGui::BeginPopupModal("Kick?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* who = s_kickCid;
        for (int i = 0; i < sn.memberCount; i++)
            if (strcmp(sn.members[i].cid, s_kickCid) == 0)
                who = sn.members[i].name;
        ImGui::Text("Kick %s from the room?", who);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(6)));
        if (ImGui::Button("Kick", ImVec2(S(100), 0))) {
            PanelSE(0);
            CmdKick(s_kickCid);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(S(100), 0))) {
            PanelSE(1);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
    return ly > ry - S(8) ? ly : ry - S(8);
}

/* ---------------- Settings tab ---------------- */
static float SettingsTab(const Snapshot& sn, ImVec2 a, ImVec2 b) {
    (void)b;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = a.y;

    SectionHdr(dl, ImVec2(a.x, y), "SERVER");
    y += S(28);
    /* server mode: three slanted pills */
    int mode = Config::mpServerMode == "cloud" ? 2 :
               Config::mpServerMode == "off" ? 0 : 1;
    const char* mnames[3] = { "OFF", "LOCAL", "ONLINE" };
    for (int i = 0; i < 3; i++) {
        ImVec2 pa(a.x + S(20) + i * S(120), y),
               pb(pa.x + S(110), pa.y + S(30));
        if (DivaBtn(pa, pb, mnames[i],
                    i == mode ? BT_TEAL : BT_DARK, true, 0.95f) && i != mode) {
            PanelSE(2);
            mp::SetServerMode(i == 2 ? kServerCloud :
                              i == 1 ? kServerLocal : kServerOff);
        }
    }
    y += S(44);
    if (mode == 2) {
        if (!s_urlInit) {
            strncpy(s_urlBuf, Config::mpCloudUrl.c_str(), sizeof(s_urlBuf) - 1);
            s_urlInit = true;
        }
        Txt(dl, ImVec2(a.x, y + S(4)), "ONLINE ADDRESS", 0.85f, kMuted);
        bool ap = DivaInput(ImVec2(a.x + S(140), y), S(280), "##url",
                            s_urlBuf, sizeof(s_urlBuf),
                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (DivaBtn(ImVec2(a.x + S(430), y), ImVec2(a.x + S(500), y + S(26)),
                    "Apply", BT_DARK, true, 0.85f) || ap) {
            PanelSE(0);
            mp::SetCloudUrl(s_urlBuf);
        }
        y += S(40);
    } else {
        char li[96];
        snprintf(li, sizeof(li), "Local: LAN discovery + server_ip %s:%d",
                 Config::mpServerIp.c_str(), Config::mpServerPort);
        Txt(dl, ImVec2(a.x + S(4), y), li, 0.85f, kDim);
        y += S(32);
    }

    SectionHdr(dl, ImVec2(a.x, y), "NOTIFICATIONS");
    y += S(28);
    Txt(dl, ImVec2(a.x + S(20), y + S(4)), "Style", 0.9f, kText);
    ImGui::SetCursorScreenPos(ImVec2(a.x + S(140), y));
    ImGui::SetNextItemWidth(S(160));
    const char* styles[] = { "Portraits", "Text", "Off" };
    int st = Config::mpNotifyStyle == "text" ? 1 :
             Config::mpNotifyStyle == "off" ? 2 : 0;
    if (ImGui::Combo("##notify", &st, styles, 3)) {
        PanelSE(2);
        Config::mpNotifyStyle = st == 1 ? "text" : st == 2 ? "off" : "avatars";
        Config::Save();
    }
    y += S(38);
    ImGui::SetCursorScreenPos(ImVec2(a.x + S(20), y));
    bool ra = Config::mpRemoteAvatars;
    if (ImGui::Checkbox("Custom portraits", &ra)) {
        PanelSE(2);
        Config::mpRemoteAvatars = ra;
        Config::Save();
    }
    y += S(28);
    ImGui::SetCursorScreenPos(ImVec2(a.x + S(20), y));
    bool chip = Config::mpShowStatus;
    if (ImGui::Checkbox("Status chip", &chip)) {
        PanelSE(2);
        Config::mpShowStatus = chip;
        Config::Save();
    }
    y += S(36);

    SectionHdr(dl, ImVec2(a.x, y), "AUTOPLAY");
    y += S(28);
    ImGui::SetCursorScreenPos(ImVec2(a.x + S(20), y));
    bool apOn = AutoPlay::g_enabled;
    if (ImGui::Checkbox("##autoplay", &apOn)) {
        PanelSE(2);
        AutoPlay::g_enabled = apOn;
        ReportAutoPlay(apOn);
    }
    Txt(dl, ImVec2(a.x + S(52), y + S(4)), "Enable AutoPlay", 0.9f, kText);
    y += S(34);
    if (AutoPlay::g_enabled) {
        static const char* const kApModes[] = {
            "Custom", "Auto", "Pro", "97", "Streak", "Human", "Clutch", "Tie"
        };
        Txt(dl, ImVec2(a.x + S(20), y + S(4)), "Mode", 0.9f, kText);
        ImGui::SetCursorScreenPos(ImVec2(a.x + S(140), y));
        int am = AutoPlay::g_mode;
        if (am < 0 || am >= AutoPlay::Mode_Count) am = AutoPlay::Mode_Auto;
        ImGui::SetNextItemWidth(S(160));
        if (ImGui::Combo("##apmode", &am, kApModes, AutoPlay::Mode_Count)) {
            PanelSE(2);
            AutoPlay::g_mode = am;
            ReportAutoPlay(true);
        }
        y += S(38);
        if (AutoPlay::g_mode == AutoPlay::Mode_Custom) {
            Txt(dl, ImVec2(a.x + S(20), y + S(4)), "Hit %", 0.9f, kText);
            ImGui::SetCursorScreenPos(ImVec2(a.x + S(140), y));
            int pct = AutoPlay::g_hitPct;
            ImGui::SetNextItemWidth(S(220));
            if (ImGui::SliderInt("##appct", &pct, 0, 100)) {
                PanelSE(2);
                AutoPlay::g_hitPct = pct;
                ReportAutoPlay(true);
            }
            y += S(34);
        }
    }
    (void)sn;
    return y;
}

/* ---------------- main panel ---------------- */
static bool s_dragging = false;

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
    s_sc = io.DisplaySize.y / 1080.0f;
    if (s_sc < 1.0f) s_sc = 1.0f;
    ImVec2 sz(S(1160), S(620));
    ImGui::SetNextWindowSize(sz, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(760), S(320)),
                                        ImVec2(io.DisplaySize.x,
                                               io.DisplaySize.y));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f,
                                   io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    Snapshot sn;
    GetSnapshot(&sn);

    /* DIVA chrome colours for the stock widgets that remain (combos,
     * checkboxes, modal) */
    const ImVec4 kWin(0.040f, 0.086f, 0.150f, 0.96f);   /* deep navy */
    const ImVec4 kFrame(0.070f, 0.135f, 0.210f, 0.95f);
    const ImVec4 kTeal4(0.224f, 0.773f, 0.733f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kWin);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.31f, 0.82f, 0.86f, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.03f, 0.06f, 0.12f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.07f, 0.16f, 0.26f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.05f, 0.12f, 0.20f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kTeal4.x, kTeal4.y, kTeal4.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(kTeal4.x, kTeal4.y, kTeal4.z, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kTeal4.x, kTeal4.y, kTeal4.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Button, kFrame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.82f, 0.77f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kTeal4);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.05f, 0.10f, 0.17f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, kTeal4);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.18f, 0.62f, 0.58f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kTeal4);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(kTeal4.x, kTeal4.y, kTeal4.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(kTeal4.x, kTeal4.y, kTeal4.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0.30f, 0.82f, 0.77f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, kTeal4);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.96f, 0.99f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.55f, 0.60f, 0.68f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8), S(5)));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    bool popen = ImGui::Begin("##mppanel", nullptr, flags);
    if (popen) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ImVec2 wr(wp.x + ws.x, wp.y + ws.y);

        /* light backdrop */
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 90));

        /* ===== header: slanted blue->cyan banner (Rhythm Game bar) ===== */
        float hh = S(58);
        SlantGradH(dl, wp, ImVec2(wr.x, wp.y + hh), -S(26), kHdrL, kHdrR);
        /* dark rim along the bottom edge */
        SlantGrad(dl, ImVec2(wp.x, wp.y + hh - S(3)),
                  ImVec2(wr.x, wp.y + hh), -S(26),
                  IM_COL32(10, 26, 66, 255), IM_COL32(8, 20, 52, 255));
        TxtBig(dl, ImVec2(wp.x + S(19), wp.y + (hh - S(30)) * 0.5f + S(1)),
               "MULTIPLAYER", S(30), IM_COL32(8, 16, 40, 200));
        TxtBig(dl, ImVec2(wp.x + S(18), wp.y + (hh - S(30)) * 0.5f),
               "MULTIPLAYER", S(30), IM_COL32_WHITE);

        float hx = wr.x - S(16);
        /* close X */
        {
            float r = S(13);
            ImVec2 cc(hx - r, wp.y + hh * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(cc.x - r, cc.y - r));
            bool xc = ImGui::InvisibleButton("##x", ImVec2(r * 2, r * 2));
            bool xh = ImGui::IsItemHovered();
            dl->AddCircleFilled(cc, r, xh ? IM_COL32(200, 60, 75, 255)
                                          : IM_COL32(30, 46, 66, 235), 24);
            dl->AddCircle(cc, r, xh ? kPink : kLineTeal, 24, 1.4f);
            float d2 = r * 0.42f;
            dl->AddLine(ImVec2(cc.x - d2, cc.y - d2), ImVec2(cc.x + d2, cc.y + d2),
                        xh ? IM_COL32_WHITE : kMuted, 2.0f);
            dl->AddLine(ImVec2(cc.x - d2, cc.y + d2), ImVec2(cc.x + d2, cc.y - d2),
                        xh ? IM_COL32_WHITE : kMuted, 2.0f);
            if (xc) { SetOverlayOpen(false); ImGui::End();
                      ImGui::PopStyleVar(5); ImGui::PopStyleColor(22); return; }
            hx -= r * 2 + S(8);
        }
        /* leave room button (in a room, before the X) */
        if (sn.code[0]) {
            float bw = S(104), bh2 = S(30);
            ImVec2 la(hx - bw, wp.y + (hh - bh2) * 0.5f), lb(la.x + bw, la.y + bh2);
            if (DivaBtn(la, lb, s_confirmLeave ? "Confirm?" : "Leave room",
                        BT_DANGER, true, 0.85f)) {
                if (s_confirmLeave) {
                    s_confirmLeave = false;
                    PanelSE(1);
                    CmdLobbyLeave();
                } else {
                    s_confirmLeave = true;
                    PanelSE(2);
                }
            }
            hx -= bw + S(10);
            if (s_confirmLeave && ImGui::IsMouseClicked(0) &&
                !ImGui::IsItemHovered() &&
                !(io.MousePos.x >= la.x - S(10) && io.MousePos.x <= lb.x &&
                  io.MousePos.y >= la.y && io.MousePos.y <= lb.y))
                s_confirmLeave = false;
        }
        /* room chip: ROOM VZAB + copy */
        if (sn.code[0]) {
            char rb[24];
            snprintf(rb, sizeof(rb), "ROOM %s", sn.code);
            float cw = TxtBigW(rb, S(20)) + S(26);
            float ch2 = S(30);
            ImVec2 ca(hx - cw, wp.y + (hh - ch2) * 0.5f),
                   cb(ca.x + cw, ca.y + ch2);
            float sk = ch2 * 0.35f;
            SlantGrad(dl, ca, cb, sk, IM_COL32(18, 34, 64, 240),
                      IM_COL32(10, 22, 48, 240));
            SlantLine(dl, ca, cb, sk, IM_COL32(200, 220, 255, 150), 1.4f);
            dl->AddText(FontLoader::Big() ? FontLoader::Big() : ImGui::GetFont(),
                        S(20), ImVec2(ca.x + S(14), ca.y + (ch2 - S(20)) * 0.5f - S(1)),
                        kTealHi, rb);
            hx -= cw + S(6);
            /* copy chip */
            float cw2 = S(52);
            ImVec2 cca(hx - cw2, ca.y), ccb(cca.x + cw2, cb.y);
            if (DivaBtn(cca, ccb, "Copy", BT_DARK, true, 0.85f)) {
                PanelSE(2);
                ImGui::SetClipboardText(sn.code);
            }
            hx -= cw2 + S(12);
            /* round number */
            char rd[16];
            snprintf(rd, sizeof(rd), "round %d", sn.round);
            Txt(dl, ImVec2(hx - TxtW(rd, 0.85f), wp.y + (hh - ImGui::GetFontSize() * 0.85f) * 0.5f),
                rd, 0.85f, kDim);
        }

        /* ===== tab pills ===== */
        float ty = wp.y + hh + S(8);
        const char* tabs[2] = { "LOBBY", "SETTINGS" };
        for (int i = 0; i < 2; i++) {
            ImVec2 ta(wp.x + S(18) + i * S(130), ty),
                   tb(ta.x + S(118), ta.y + S(30));
            if (DivaBtn(ta, tb, tabs[i], i == s_tab ? BT_YELLOW : BT_GREY,
                        true, 0.95f) && i != s_tab) {
                s_tab = i;
                PanelSE(2);
            }
        }

        /* header drag (grab anywhere on the bar that isn't a control) */
        {
            ImVec2 da(wp.x, wp.y), db(wr.x, wp.y + hh);
            bool hovHdr = io.MousePos.x >= da.x && io.MousePos.x <= db.x &&
                          io.MousePos.y >= da.y && io.MousePos.y <= db.y;
            if (hovHdr && ImGui::IsMouseClicked(0) &&
                !ImGui::IsAnyItemHovered()) {
                s_dragging = true;
            }
            if (s_dragging) {
                if (ImGui::IsMouseDown(0)) {
                    ImVec2 d = ImGui::GetMouseDragDelta(0);
                    ImVec2 np(wp.x + d.x, wp.y + d.y);
                    float mg = S(60);
                    if (np.x < mg - ws.x) np.x = mg - ws.x;
                    if (np.x > io.DisplaySize.x - mg) np.x = io.DisplaySize.x - mg;
                    if (np.y < 0.0f) np.y = 0.0f;
                    if (np.y > io.DisplaySize.y - mg) np.y = io.DisplaySize.y - mg;
                    ImGui::SetWindowPos(np);
                    ImGui::ResetMouseDragDelta(0);
                } else s_dragging = false;
            }
        }

        /* ===== content ===== */
        ImVec2 ca(wp.x + S(22), ty + S(40)),
               cb(wr.x - S(22), wr.y - S(46));
        float cbot = (s_tab == 0) ? LobbyTab(sn, ca, cb)
                                  : SettingsTab(sn, ca, cb);
        /* fit the window to its content */
        float want = cbot - wp.y + S(48);
        if (want < S(300)) want = S(300);
        float maxh = io.DisplaySize.y - S(40);
        if (want > maxh) want = maxh;
        float wantW = (s_tab == 0 && sn.code[0]) ? S(1160) : S(640);
        if (fabsf(ws.y - want) > S(24) || fabsf(ws.x - wantW) > S(24))
            ImGui::SetWindowSize(ImVec2(wantW, want));

        /* ===== footer (Switch DrawRoom priority) ===== */
        float fy = wr.y - S(38);
        dl->AddLine(ImVec2(wp.x + S(18), fy), ImVec2(wr.x - S(18), fy),
                    kLineTeal, 1.0f);
        char foot[220];
        ImU32 footCol = kMuted;
        if (sn.code[0]) {
            const char* leadName = "";
            for (int i = 0; i < sn.memberCount; i++)
                if (strcmp(sn.members[i].cid, sn.leaderCid) == 0)
                    leadName = sn.members[i].name;
            if (!sn.connected) {
                int left = sn.offlineMs >= 0 ? 30 - sn.offlineMs / 1000 : 30;
                snprintf(foot, sizeof(foot),
                         "Connection lost - reconnecting (leaving the room in %ds)",
                         left < 0 ? 0 : left);
                footCol = kYellow;
            } else if (sn.voteKind[0])
                snprintf(foot, sizeof(foot), "%s vote: %d/%d agreed (%ds left)",
                         strcmp(sn.voteKind, "pause") == 0 ? "Pause" : "Resume",
                         sn.voteAgreed, sn.voteNeed, sn.voteEndsIn);
            else if (sn.groupPaused)
                snprintf(foot, sizeof(foot), "GROUP PAUSED");
            else if (sn.notReady[0])
                snprintf(foot, sizeof(foot), "Not ready: %s", sn.notReady);
            else if (sn.statusLine[0] && sn.phase != 1)
                snprintf(foot, sizeof(foot), "%s", sn.statusLine);
            else {
                const char* mode = strcmp(sn.pickMode, "owner") == 0 ? "Owner picks"
                                 : (strcmp(sn.pickMode, "assigned") == 0 ? "Assigned" : "Rotate");
                if (strcmp(sn.leaderCid, sn.myCid) == 0)
                    snprintf(foot, sizeof(foot), "You're picking the song  |  %s", mode);
                else
                    snprintf(foot, sizeof(foot), "%s is picking the song  |  %s",
                             leadName[0] ? leadName : "The leader", mode);
            }
        } else {
            MpStatus mst;
            mp::GetStatus(&mst);
            if (mst.state == MpState::Connected)
                snprintf(foot, sizeof(foot), "%s  %s  %d ms",
                         mp::StateName(mst.state), mst.server, mst.rtt_ms);
            else if (mst.last_error[0])
                snprintf(foot, sizeof(foot), "%s  %s  (%s)",
                         mp::StateName(mst.state), mst.server, mst.last_error);
            else
                snprintf(foot, sizeof(foot), "%s  %s",
                         mp::StateName(mst.state), mst.server);
        }
        Txt(dl, ImVec2(wp.x + S(22), fy + S(9)), foot, 0.88f, footCol);
        /* own status + F8 hint, right side */
        float rx = wr.x - S(22);
        Txt(dl, ImVec2(rx - TxtW("F8 close", 0.82f), fy + S(10)), "F8 close",
            0.82f, kDim);
        rx -= TxtW("F8 close", 0.82f) + S(14);
        if (sn.code[0])
            for (int i = 0; i < sn.memberCount; i++)
                if (strcmp(sn.members[i].cid, sn.myCid) == 0) {
                    const char* ms = MemberStatus(sn.members[i]);
                    StatusDot(dl, ImVec2(rx - TxtW(ms, 0.88f) - S(9),
                                         fy + S(15)), S(4), MemberDot(sn.members[i]));
                    Txt(dl, ImVec2(rx - TxtW(ms, 0.88f), fy + S(9)), ms,
                        0.88f, MemberDot(sn.members[i]));
                    break;
                }
        ImGui::End();
    } else {
        ImGui::End();
    }
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(22);
}

} // namespace mp::lobby
