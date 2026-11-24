// PC panel for the MP lobby, full screen in the style of the game's own
// Options screens (blue header tab, light slanted sheet, white hex-ended
// rows with value pills, help band + title tag, key-cap footer). Two pages:
// Room and Settings; the players of the room sit right of the sheet.
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
#include <algorithm>

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
    if (!m.connected) return "Disconnected";
    if (m.nosong) return "No song";
    if (m.ready) return "Ready";
    if (strcmp(m.reason, "modules") == 0) return "Choosing modules";
    if (strcmp(m.reason, "wrong_song") == 0) return "Wrong song";
    if (strcmp(m.reason, "difficulty") == 0) return "Difficulty";
    if (strcmp(m.reason, "not_in_sel") == 0) return "Not in song select";
    return "Not ready";
}
static ImU32 MemberDot(const MemberInfo& m) {
    return !m.connected ? kDim : (m.nosong ? kRed : (m.ready ? kGreen : kYellow));
}
/* darker status colours that read on the light member plates */
static ImU32 MemberDotCard(const MemberInfo& m) {
    return !m.connected ? kDotGrey
         : (m.nosong ? IM_COL32(214, 40, 84, 255) : (m.ready ? kDotGreen : kDotAmber));
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

/* the shape the draw list's path holds (a convex outline around hc) filled
 * with col, except a round hole (hc, hr) that stays open. A fan of quads
 * from the hole to the outline, split at the outline's own corners so its
 * edges stay exact; the fill is not anti-aliased (abutting AA quads leave
 * seams), the two edges get a 1 px stroke instead, so the path is expected
 * half a pixel inside the wanted outline and the fill stops half a pixel
 * short of the hole. Clears the path. */
static void PathFillWithHole(ImDrawList* dl, ImVec2 hc, float hr, ImU32 col) {
    hr += 0.5f;
    ImVec2 pts[64];
    int n = dl->_Path.Size < 64 ? dl->_Path.Size : 64;
    for (int i = 0; i < n; i++) pts[i] = dl->_Path[i];
    dl->PathClear();
    if (n < 3) return;
    float ang[96];
    int na = 0;
    for (int i = 0; i < n; i++) ang[na++] = atan2f(pts[i].y - hc.y, pts[i].x - hc.x);
    for (int i = 0; i < 24; i++) ang[na++] = -3.14159265f + i * (6.2831853f / 24.0f);
    std::sort(ang, ang + na);
    auto outer = [&](float t, ImVec2* out) {
        const ImVec2 d(cosf(t), sinf(t));
        float best = -1.0f;
        for (int i = 0; i < n; i++) {
            const ImVec2 p = pts[i], q = pts[(i + 1) % n];
            const ImVec2 e(q.x - p.x, q.y - p.y), w(p.x - hc.x, p.y - hc.y);
            const float cr = d.x * e.y - d.y * e.x;
            if (fabsf(cr) < 1e-6f) continue;
            const float tt = (w.x * e.y - w.y * e.x) / cr, u = (w.x * d.y - w.y * d.x) / cr;
            if (tt > best && u >= -0.001f && u <= 1.001f) best = tt;
        }
        if (best <= 0.0f) return false;
        *out = ImVec2(hc.x + d.x * best, hc.y + d.y * best);
        return true;
    };
    const ImDrawListFlags keep = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int i = 0; i < na; i++) {
        const float t0 = ang[i], t1 = i + 1 < na ? ang[i + 1] : ang[0] + 6.2831853f;
        if (t1 - t0 < 1e-5f) continue;
        ImVec2 p0, p1;
        if (!outer(t0, &p0) || !outer(t1, &p1)) continue;
        dl->AddQuadFilled(ImVec2(hc.x + cosf(t0) * hr, hc.y + sinf(t0) * hr), p0, p1,
                          ImVec2(hc.x + cosf(t1) * hr, hc.y + sinf(t1) * hr), col);
    }
    dl->Flags = keep;
    dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 1.0f);
    dl->AddCircle(hc, hr, col, 20, 1.0f);
}

/* which version of the mod a member runs: a Joy-Con pair for the Switch
 * game (Mega Mix), a monitor for the PC game (Mega Mix+). h tall, centred
 * on c; cut = the colour behind it for the screen / stick cut-outs, or 0
 * to leave them open (the icon sits on a backdrop, not on a flat plate). */
static float IconPlatW(float h, int plat) {
    return plat == PLAT_PC ? h * 1.22f : (plat == PLAT_SWITCH ? h * 0.90f : 0.0f);
}
static void IconPlat(ImDrawList* dl, ImVec2 c, float h, int plat, ImU32 col, ImU32 cut) {
    const bool open = (cut >> IM_COL32_A_SHIFT) == 0;
    if (plat == PLAT_PC) {
        const float w = h * 1.22f, sh = h * 0.70f;
        ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y - h * 0.5f + sh);
        const float in = h * 0.115f;
        if (open) {     /* the bezel alone: a stroke as thick as the bezel */
            dl->AddRect(ImVec2(a.x + in * 0.5f, a.y + in * 0.5f), ImVec2(b.x - in * 0.5f, b.y - in * 0.5f), col,
                        h * 0.12f - in * 0.5f, 0, in);
        } else {
            dl->AddRectFilled(a, b, col, h * 0.12f);
            dl->AddRectFilled(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), cut, h * 0.04f);
        }
        dl->AddRectFilled(ImVec2(c.x - h * 0.09f, b.y), ImVec2(c.x + h * 0.09f, c.y + h * 0.40f), col);
        dl->AddRectFilled(ImVec2(c.x - h * 0.34f, c.y + h * 0.36f), ImVec2(c.x + h * 0.34f, c.y + h * 0.5f),
                          col, h * 0.06f);
    } else if (plat == PLAT_SWITCH) {
        const float w = h * 0.90f, gap = h * 0.09f, r = h * 0.24f;
        ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
        const float hw = (w - gap) * 0.5f;
        const ImVec2 s0(a.x + hw * 0.52f, a.y + h * 0.30f), s1(b.x - hw * 0.52f, a.y + h * 0.62f);
        if (open) {     /* each half around its stick */
            const float o = 0.5f;
            dl->PathRect(ImVec2(a.x + o, a.y + o), ImVec2(c.x - gap * 0.5f - o, b.y - o), r - o,
                         ImDrawFlags_RoundCornersLeft);
            PathFillWithHole(dl, s0, h * 0.11f, col);
            dl->PathRect(ImVec2(c.x + gap * 0.5f + o, a.y + o), ImVec2(b.x - o, b.y - o), r - o,
                         ImDrawFlags_RoundCornersRight);
            PathFillWithHole(dl, s1, h * 0.11f, col);
        } else {
            dl->AddRectFilled(a, ImVec2(c.x - gap * 0.5f, b.y), col, r, ImDrawFlags_RoundCornersLeft);
            dl->AddRectFilled(ImVec2(c.x + gap * 0.5f, a.y), b, col, r, ImDrawFlags_RoundCornersRight);
            dl->AddCircleFilled(s0, h * 0.11f, cut, 12);
            dl->AddCircleFilled(s1, h * 0.11f, cut, 12);
        }
    }
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

/* ---------------- Lobby tab ---------------- */
static char s_nameBuf[40] = "";
static bool s_nameInit = false;
static char s_codeBuf[8] = "";
/* Join Room keeps showing what became of the code as it was submitted,
 * until it is edited: a line for the help band + a marked field */
static char  s_codeSent[8] = "";
static bool  s_joinShown = false;
static char  s_joinNote[80] = "";
static ImU32 s_joinNoteCol = 0;
static bool  s_joinBad = false;
static char s_urlBuf[160] = "";
static bool s_urlInit = false;
static bool s_confirmLeave = false;
static int  s_tab = 0;          /* 0 lobby, 1 settings */

/* white text with a coloured outline, centred on cx (podium captions on
 * the panel's backdrop) */
static void PodiumText(ImDrawList* dl, ImFont* f, float px, float cx, float y, const char* s,
                       ImU32 fill, ImU32 outl) {
    ImVec2 p(cx - f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x * 0.5f, y);
    const float o = px * 0.075f;
    for (int k = 0; k < 8; k++) {
        float ang = k * 0.785398f;
        dl->AddText(f, px, ImVec2(p.x + cosf(ang) * o, p.y + sinf(ang) * o), outl, s);
    }
    dl->AddText(f, px, ImVec2(p.x, p.y + o * 0.8f), outl, s);
    dl->AddText(f, px, p, fill, s);
}
/* podium: the session's top three in a row, the hexagons standing on one
 * line so the plates, names and role captions line up. No status dot and
 * no trophies (the list below has both); the platform icon's cut-outs are
 * left open. Returns the bottom y. */
static float DrawPodium(ImDrawList* dl, const Snapshot& sn, const int* order,
                        ImVec2 a, ImVec2 b) {
    int n = sn.memberCount < 3 ? sn.memberCount : 3;
    float cw = (b.x - a.x) / 3.0f;
    float xoff = (3.0f - n) * cw * 0.5f;
    const float rads[3] = { S(66), S(56), S(48) };
    const float tip = a.y + rads[0] * 2.0f + S(4);     /* shared bottom tip */
    const ImU32 outl = IM_COL32(0, 96, 150, 255);      /* as the room code */
    ImFont* bf = FontLoader::Big() ? FontLoader::Big() : ImGui::GetFont();
    float bot = a.y;
    for (int i = 0; i < n; i++) {
        const MemberInfo& m = sn.members[order[i]];
        float cx = a.x + xoff + cw * (i + 0.5f);
        float r = rads[i];
        bool you = strcmp(m.cid, sn.myCid) == 0;
        DrawHexAvatar(dl, ImVec2(cx, tip - r), r, MemberRef(sn, m), RaceColor(sn, m.cid),
                      m.connected, you);
        /* numbered white plate over the hexagon's bottom tip */
        float pw = S(i == 0 ? 46.0f : 42.0f), ph = S(i == 0 ? 36.0f : 33.0f);
        ImVec2 pa(cx - pw * 0.5f, tip - S(21)), pb(pa.x + pw, pa.y + ph);
        dl->AddRectFilled(pa, pb, IM_COL32(248, 248, 250, 255), S(4));
        dl->AddRect(pa, pb, kCardInk, S(4), 0, S(1.6f));
        char num[4]; snprintf(num, sizeof(num), "%d", i + 1);
        const float npx = S(i == 0 ? 30.0f : 26.0f);
        ImVec2 ns = bf->CalcTextSizeA(npx, FLT_MAX, 0.0f, num);
        dl->AddText(bf, npx, ImVec2(cx - ns.x * 0.5f, (pa.y + pb.y - ns.y) * 0.5f), kCardInk, num);
        /* name (+ platform icon), then the role caption */
        const char* nm = m.name[0] ? m.name : "(unnamed)";
        ImFont* nf = bf;                 /* the bold face is Latin only */
        for (const unsigned char* p = (const unsigned char*)nm; *p; p++)
            if (*p > 126 || *p < 32) { nf = ImGui::GetFont(); break; }
        const float fpx = S(30), ih = S(20), iw = IconPlatW(ih, m.plat);
        const float icw = iw > 0 ? iw + S(8) : 0.0f;
        char nb[64];
        snprintf(nb, sizeof(nb), "%s", nm);
        for (int len = (int)strlen(nb);
             len > 0 && nf->CalcTextSizeA(fpx, FLT_MAX, 0.0f, nb).x > cw - S(16) - icw;) {
            do { len--; } while (len > 0 && ((unsigned char)nb[len] & 0xC0) == 0x80);
            snprintf(nb + len, sizeof(nb) - len, "..");
        }
        const float nw = nf->CalcTextSizeA(fpx, FLT_MAX, 0.0f, nb).x;
        const ImU32 fill = m.connected ? IM_COL32_WHITE : IM_COL32(198, 214, 222, 255);
        float ny = pb.y + S(3);
        PodiumText(dl, nf, fpx, cx - icw * 0.5f, ny, nb, fill, outl);
        if (iw > 0)
            IconPlat(dl, ImVec2(cx + (nw + icw) * 0.5f - iw * 0.5f, ny + fpx * 0.56f), ih, m.plat, fill, 0);
        ny += fpx + S(4);
        char cap[48];
        RoleCaption(sn, m, cap, sizeof(cap));
        const float cpx = S(21);
        if (cap[0]) PodiumText(dl, bf, cpx, cx, ny, cap, IM_COL32_WHITE, outl);
        float bb = ny + cpx + S(6);      /* the caption line is always kept */
        if (bb > bot) bot = bb;
    }
    return bot;
}

/* ================================================================
 * Panel in the style of the game's own Options screens: blue header tab,
 * light slanted body with a blue foot strip, white hex-ended rows that
 * turn teal when pointed at, grey value pills with arrows, the dark band
 * with a slanted title tag and the key-cap footer. Colours sampled from
 * Options > Display Settings (MM+ 1.03).
 * ============================================================== */
static const ImU32 gBlue      = IM_COL32(0, 141, 213, 255);    /* header tab / foot strip */
static const ImU32 gBody0     = IM_COL32(196, 238, 243, 224);  /* light body */
static const ImU32 gBody1     = IM_COL32(176, 244, 248, 224);
static const ImU32 gRow0      = IM_COL32(255, 255, 255, 255);  /* white row */
static const ImU32 gRow1      = IM_COL32(238, 240, 241, 255);
static const ImU32 gRowShadow = IM_COL32(120, 170, 180, 120);
static const ImU32 gRowText   = IM_COL32(139, 141, 147, 255);  /* grey label */
static const ImU32 gRowOff    = IM_COL32(198, 201, 205, 255);  /* label of a row that can't be used */
static const ImU32 gWarn      = IM_COL32(255, 232, 120, 255);  /* warning text on the help band */
static const ImU32 gBad       = IM_COL32(226, 44, 96, 255);    /* refused input */
static const ImU32 gInk       = IM_COL32(74, 80, 92, 255);     /* dark text on white */
static const ImU32 gSel0      = IM_COL32(0, 208, 214, 255);    /* selected teal */
static const ImU32 gSel1      = IM_COL32(0, 186, 192, 255);
static const ImU32 gSelEdge   = IM_COL32(180, 252, 252, 255);
static const ImU32 gSelDark   = IM_COL32(12, 134, 140, 255);
static const ImU32 gPill      = IM_COL32(165, 165, 165, 255);  /* value pill */
static const ImU32 gPillSel   = IM_COL32(16, 154, 158, 255);
static const ImU32 gSlot      = IM_COL32(227, 227, 227, 255);  /* inset slot */
static const ImU32 gBand      = IM_COL32(0, 142, 161, 205);    /* help band */
static const ImU32 gTitleTag  = IM_COL32(0, 169, 254, 255);
static const ImU32 gFooter    = IM_COL32(1, 68, 100, 240);
static const ImU32 gKeyCap    = IM_COL32(0, 142, 200, 255);
static const ImU32 gKeyText   = IM_COL32(2, 220, 251, 255);
static const ImU32 gPinkTag   = IM_COL32(241, 80, 175, 255);

static float s_ox = 0.0f;                 /* left edge of the 16:9 layout */
static const char* s_help = nullptr;      /* help line of the row under the pointer */
static inline float X(float v) { return s_ox + v * s_sc; }

static ImFont* BoldF() { return FontLoader::Big() ? FontLoader::Big() : ImGui::GetFont(); }
/* names can be non-Latin: the bold face only has the Latin ranges */
static ImFont* FontFor(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; *p; p++)
        if (*p > 126 || *p < 32) return ImGui::GetFont();
    return BoldF();
}
static float TW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}
static void T(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 col, const char* s) {
    dl->AddText(f, px, p, col, s);
}
/* white text with the game's dark-teal outline (selected rows, titles) */
static void TOut(ImDrawList* dl, ImFont* f, float px, ImVec2 p, const char* s,
                 ImU32 out = gSelDark, ImU32 col = IM_COL32_WHITE) {
    const float o = px * 0.07f;
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f;
        dl->AddText(f, px, ImVec2(p.x + cosf(a) * o, p.y + sinf(a) * o), out, s);
    }
    dl->AddText(f, px, ImVec2(p.x, p.y + o * 0.8f), out, s);
    dl->AddText(f, px, p, col, s);
}
/* s trimmed with ".." to fit w (UTF-8 safe) */
static const char* Fit(ImFont* f, float px, const char* s, float w, char* out, int n) {
    snprintf(out, n, "%s", s);
    int len = (int)strlen(out);
    while (len > 0 && TW(f, px, out) > w) {
        do { len--; } while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        snprintf(out + len, n - len, "..");
    }
    return out;
}
static ImU32 Mix(ImU32 a, ImU32 b, float t) {
    ImU32 o = 0;
    for (int s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        o |= (ImU32)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return o;
}
/* the game's row shape: a bar with pointed ends, vertical gradient */
static void HexBar(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bot, float grow = 0.0f) {
    a.x -= grow; a.y -= grow; b.x += grow; b.y += grow;
    const float h = b.y - a.y, e = h * 0.30f, my = (a.y + b.y) * 0.5f;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 mid = Mix(top, bot, 0.5f);
    dl->PrimReserve(12, 6);
    const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(ImVec2(a.x, my), uv, mid);
    dl->PrimWriteVtx(ImVec2(a.x + e, a.y), uv, top);
    dl->PrimWriteVtx(ImVec2(b.x - e, a.y), uv, top);
    dl->PrimWriteVtx(ImVec2(b.x, my), uv, mid);
    dl->PrimWriteVtx(ImVec2(b.x - e, b.y), uv, bot);
    dl->PrimWriteVtx(ImVec2(a.x + e, b.y), uv, bot);
    for (int i = 1; i < 5; i++) {
        dl->PrimWriteIdx(i0);
        dl->PrimWriteIdx((ImDrawIdx)(i0 + i));
        dl->PrimWriteIdx((ImDrawIdx)(i0 + i + 1));
    }
}
/* a menu row: white with a soft shadow, or the teal selected look */
static void RowPlate(ImDrawList* dl, ImVec2 a, ImVec2 b, bool sel) {
    if (sel) {
        HexBar(dl, a, b, gSelEdge, gSelEdge, S(6));
        HexBar(dl, a, b, gSelDark, gSelDark, S(2.5f));
        HexBar(dl, a, b, gSel0, gSel1);
    } else {
        HexBar(dl, ImVec2(a.x + S(2), a.y + S(5)), ImVec2(b.x + S(2), b.y + S(5)), gRowShadow, gRowShadow);
        HexBar(dl, a, b, gRow0, gRow1);
    }
}
static void RowLabel(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* label, bool sel, float px = 0.0f,
                     ImU32 col = gRowText) {
    if (px <= 0.0f) px = S(38);
    ImFont* f = BoldF();
    ImVec2 p(a.x + S(46), (a.y + b.y) * 0.5f - px * 0.5f - S(1));
    if (sel) TOut(dl, f, px, p, label);
    else T(dl, f, px, p, col, label);
}
static void Tri(ImDrawList* dl, ImVec2 c, float r, bool left, ImU32 col) {
    const float d = left ? -1.0f : 1.0f;
    dl->AddTriangleFilled(ImVec2(c.x + d * r, c.y), ImVec2(c.x - d * r * 0.6f, c.y - r * 0.9f),
                          ImVec2(c.x - d * r * 0.6f, c.y + r * 0.9f), col);
}
static const float kRowH = 72.0f;

/* "Label   < value >" row. Returns -1 / +1 when clicked (left / right half
 * of the value), 0 otherwise. Not enabled = shows the value only. */
static int OptRow(const char* id, ImVec2 a, float w, const char* label, const char* value,
                  const char* help, bool enabled = true) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + w, a.y + S(kRowH));
    ImGui::SetCursorScreenPos(a);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, S(kRowH)));
    bool hov = ImGui::IsItemHovered();
    bool sel = hov && enabled;
    if (hov && help) s_help = help;
    RowPlate(dl, a, b, sel);
    RowLabel(dl, a, b, label, sel);
    const float pw = w * 0.36f < S(210) ? S(210) : w * 0.36f;
    ImVec2 pa(b.x - S(64) - pw, a.y + S(15)), pb(b.x - S(64), b.y - S(15));
    HexBar(dl, pa, pb, sel ? gPillSel : gPill, sel ? gPillSel : gPill);
    char vb[64];
    ImFont* vf = FontFor(value);
    const float vpx = S(29);
    Fit(vf, vpx, value, pw - S(36), vb, sizeof(vb));
    T(dl, vf, vpx, ImVec2((pa.x + pb.x - TW(vf, vpx, vb)) * 0.5f, (pa.y + pb.y - vpx) * 0.5f - S(1)),
      IM_COL32_WHITE, vb);
    if (enabled) {
        const ImU32 ac = sel ? IM_COL32_WHITE : IM_COL32(200, 200, 200, 255);
        const float my = (a.y + b.y) * 0.5f;
        Tri(dl, ImVec2(pa.x - S(16), my), S(10), true, ac);
        Tri(dl, ImVec2(pb.x + S(16), my), S(10), false, ac);
    }
    if (!clicked || !enabled) return 0;
    return ImGui::GetIO().MousePos.x < (pa.x + pb.x) * 0.5f && ImGui::GetIO().MousePos.x > pa.x - S(34)
               ? -1 : 1;
}

/* small hex-ended button: 0 teal, 1 pink (danger), 2 white */
static bool MiniBtn(const char* id, ImVec2 a, ImVec2 b, const char* label, int kind,
                    bool enabled = true, const char* help = nullptr) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(a);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
    bool hov = enabled && ImGui::IsItemHovered();
    if (hov && help) s_help = help;
    ImU32 c0, c1, edge, tc = IM_COL32_WHITE, to;
    if (!enabled) { c0 = IM_COL32(206, 208, 210, 255); c1 = IM_COL32(190, 192, 194, 255);
                    edge = IM_COL32(170, 172, 174, 255); to = IM_COL32(150, 152, 156, 255); }
    else if (kind == 1) { c0 = hov ? IM_COL32(255, 120, 200, 255) : IM_COL32(246, 92, 184, 255);
                          c1 = hov ? IM_COL32(244, 86, 180, 255) : IM_COL32(226, 62, 160, 255);
                          edge = IM_COL32(255, 200, 232, 255); to = IM_COL32(150, 30, 100, 255); }
    else if (kind == 2) { c0 = gRow0; c1 = gRow1; edge = hov ? gSel1 : IM_COL32(200, 204, 206, 255);
                          tc = hov ? gSelDark : gRowText; to = 0; }
    else { c0 = hov ? IM_COL32(40, 226, 232, 255) : gSel0; c1 = hov ? IM_COL32(8, 204, 210, 255) : gSel1;
           edge = gSelEdge; to = gSelDark; }
    HexBar(dl, a, b, edge, edge, S(2.5f));
    HexBar(dl, a, b, c0, c1);
    ImFont* f = BoldF();
    const float px = (b.y - a.y) * 0.62f;
    ImVec2 p((a.x + b.x - TW(f, px, label)) * 0.5f, (a.y + b.y - px) * 0.5f - S(1));
    if (to) TOut(dl, f, px, p, label, to, tc);
    else T(dl, f, px, p, tc, label);
    return enabled && clicked;
}

/* "Label  [text field]  [button]" row; returns true on Enter or the button */
static bool FieldRow(const char* id, ImVec2 a, float w, const char* label, char* buf, int n,
                     ImGuiInputTextFlags flags, const char* hint, const char* btn,
                     bool btnEnabled, const char* help, float labelW = 300.0f, bool bad = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + w, a.y + S(kRowH));
    const bool over = ImGui::IsMouseHoveringRect(a, b);
    if (over && help) s_help = help;
    RowPlate(dl, a, b, false);
    RowLabel(dl, a, b, label, false);
    const float bw = btn ? S(160) : 0.0f;
    ImVec2 fa(a.x + S(46) + S(labelW), a.y + S(13)), fb(b.x - S(40) - (btn ? bw + S(14) : 0.0f), b.y - S(13));
    dl->AddRectFilled(fa, fb, gSlot, S(8));
    const float fpx = S(29);
    ImGui::PushFont(ImGui::GetFont());
    ImGui::SetWindowFontScale(fpx / ImGui::GetFont()->FontSize);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.26f, 0.29f, 0.34f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, ImVec4(0.62f, 0.64f, 0.67f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, ImVec4(0.0f, 0.76f, 0.78f, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(12), (fb.y - fa.y - fpx) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetCursorScreenPos(fa);
    ImGui::SetNextItemWidth(fb.x - fa.x);
    char iid[48];
    snprintf(iid, sizeof(iid), "##f%s", id);
    bool ret = hint ? ImGui::InputTextWithHint(iid, hint, buf, n, flags)
                    : ImGui::InputText(iid, buf, n, flags);
    const bool act = ImGui::IsItemActive();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(6);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    dl->AddRect(fa, fb, bad ? gBad : act ? gSel1 : IM_COL32(196, 200, 202, 255), S(8), 0,
                bad || act ? S(3) : S(1.5f));
    if (btn) {
        char bid[48];
        snprintf(bid, sizeof(bid), "##b%s", id);
        if (MiniBtn(bid, ImVec2(b.x - S(40) - bw, a.y + S(13)), ImVec2(b.x - S(40), b.y - S(13)), btn, 0,
                    btnEnabled, help))
            ret = true;
    }
    return ret;
}

/* full-width action row (Create Room): white, teal while pointed at */
static bool ActionRow(const char* id, ImVec2 a, float w, const char* label, const char* help,
                      bool enabled = true) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(a.x + w, a.y + S(kRowH));
    ImGui::SetCursorScreenPos(a);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, S(kRowH)));
    bool hov = enabled && ImGui::IsItemHovered();
    if (ImGui::IsItemHovered() && help) s_help = help;   /* a disabled row still says why */
    RowPlate(dl, a, b, hov);
    RowLabel(dl, a, b, label, hov, 0.0f, enabled ? gRowText : gRowOff);
    return enabled && clicked;
}

/* [key] label, right-aligned at rx on the footer; returns the new rx */
/* with an id the hint (cap and label) is a button too: *clicked when it
 * is pressed, `help` in the band while pointed at */
static float KeyHint(ImDrawList* dl, float rx, float cy, const char* key, const char* label,
                     const char* id = nullptr, const char* help = nullptr, bool* clicked = nullptr) {
    ImFont* f = BoldF();
    const float px = S(25), kh = S(32);
    const float lw = TW(f, px, label), kw = TW(f, px * 0.9f, key) + S(22);
    ImVec2 ka(rx - lw - S(10) - kw, cy - kh * 0.5f), kb(ka.x + kw, cy + kh * 0.5f);
    bool hov = false;
    if (id) {
        ImGui::SetCursorScreenPos(ImVec2(ka.x - S(8), ka.y - S(6)));
        const bool hit = ImGui::InvisibleButton(id, ImVec2(rx - ka.x + S(16), kh + S(12)));
        hov = ImGui::IsItemHovered();
        if (hov) {
            s_help = help;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (clicked) *clicked = hit;
    }
    T(dl, f, px, ImVec2(rx - lw, cy - px * 0.5f - S(1)), hov ? IM_COL32_WHITE : gKeyText, label);
    dl->AddRectFilled(ka, kb, hov ? gSel0 : gKeyCap, S(5));
    T(dl, f, px * 0.9f, ImVec2(ka.x + S(11), cy - px * 0.45f - S(1)),
      hov ? IM_COL32_WHITE : IM_COL32(170, 236, 255, 255), key);
    return ka.x - S(26);
}

/* one player: white hex row with number, portrait, name + platform icon,
 * status and tags on a second line, trophies, wins and (owner) Kick.
 * Returns true when the row itself was clicked (assign mode). */
static bool PlayerRow(ImDrawList* dl, const Snapshot& sn, const MemberInfo& m, int idx,
                      ImVec2 a, float w, bool owner, bool assign) {
    const float H = S(68);
    ImVec2 b(a.x + w, a.y + H);
    const bool you = strcmp(m.cid, sn.myCid) == 0;
    const bool canKick = owner && !you;
    bool hov = false, clk = false;
    if (assign) {
        ImGui::SetCursorScreenPos(a);
        char id[32]; snprintf(id, sizeof(id), "##prow%d", idx);
        clk = ImGui::InvisibleButton(id, ImVec2(w - (canKick ? S(140) : 0.0f), H));
        hov = ImGui::IsItemHovered();
        if (hov) s_help = "Make this player the song picker";
    }
    if (hov) RowPlate(dl, a, b, true);
    else {
        HexBar(dl, ImVec2(a.x + S(2), a.y + S(5)), ImVec2(b.x + S(2), b.y + S(5)), gRowShadow, gRowShadow);
        if (you) HexBar(dl, a, b, gSel1, gSel1, S(3));
        if (m.connected) HexBar(dl, a, b, gRow0, gRow1);
        else HexBar(dl, a, b, IM_COL32(222, 224, 226, 255), IM_COL32(204, 206, 208, 255));
    }
    const float my = (a.y + b.y) * 0.5f;
    const ImU32 plate = hov ? gSel0 : (m.connected ? gRow0 : IM_COL32(214, 216, 218, 255));
    ImFont* bf = BoldF();
    /* number + portrait */
    char num[8]; snprintf(num, sizeof(num), "%d", idx + 1);
    T(dl, bf, S(24), ImVec2(a.x + S(30), my - S(13)), hov ? IM_COL32_WHITE : gRowText, num);
    const float hr = S(25);
    ImVec2 hc(a.x + S(84), my);
    DrawHexAvatar(dl, hc, hr, MemberRef(sn, m), RaceColor(sn, m.cid), m.connected, false);
    StatusDot(dl, ImVec2(hc.x + hr * 0.68f, hc.y + hr * 0.68f), S(5), MemberDotCard(m));
    /* right side: kick, wins, trophies */
    float xr = b.x - S(30);
    if (canKick) {
        static char   s_armCid[20] = "";
        static double s_armUntil = 0.0;
        const bool armed = strcmp(s_armCid, m.cid) == 0 && ImGui::GetTime() < s_armUntil;
        char id[32]; snprintf(id, sizeof(id), "##kick%d", idx);
        if (MiniBtn(id, ImVec2(xr - S(98), my - S(19)), ImVec2(xr, my + S(19)), armed ? "Sure?" : "Kick", 1,
                    true, "Remove this player from the room")) {
            if (armed) { PanelSE(1); CmdKick(m.cid); s_armCid[0] = 0; }
            else {
                PanelSE(2);
                snprintf(s_armCid, sizeof(s_armCid), "%s", m.cid);
                s_armUntil = ImGui::GetTime() + 3.0;
            }
        }
        xr -= S(98) + S(14);
    }
    char wb[16]; snprintf(wb, sizeof(wb), "%dW", m.wins);
    const float ww = TW(bf, S(22), wb);
    T(dl, bf, S(22), ImVec2(xr - ww, my - S(12)),
      m.wins > 0 ? IM_COL32(214, 150, 0, 255) : (hov ? IM_COL32_WHITE : gRowText), wb);
    xr -= ww + S(12);
    const float th = S(16);
    const float tw = DrawTrophyRow(dl, ImVec2(0, 0), th, m.places, false, 8.0f, gInk, 1.0f);
    DrawTrophyRow(dl, ImVec2(xr - tw, my), th, m.places, true, 8.0f, hov ? IM_COL32_WHITE : gInk, 1.0f);
    xr -= tw + S(12);
    /* line 1: name + platform icon */
    const float nx = a.x + S(122);
    const char* nm = m.name[0] ? m.name : "(unnamed)";
    ImFont* nf = FontFor(nm);
    const float npx = S(25), ih = S(17), iw = IconPlatW(ih, m.plat);
    char nb[64];
    Fit(nf, npx, nm, xr - nx - (iw > 0 ? iw + S(8) : 0.0f), nb, sizeof(nb));
    if (hov) TOut(dl, nf, npx, ImVec2(nx, a.y + S(7)), nb);
    else T(dl, nf, npx, ImVec2(nx, a.y + S(7)), m.connected ? gInk : gRowText, nb);
    if (iw > 0)
        IconPlat(dl, ImVec2(nx + TW(nf, npx, nb) + S(8) + iw * 0.5f, a.y + S(7) + npx * 0.55f), ih, m.plat,
                 hov ? IM_COL32_WHITE : gRowText, plate);
    /* line 2: status, then role tags */
    const float ly = a.y + S(39), lpx = S(17);
    const char* ms = MemberStatus(m);
    ImFont* sf = ImGui::GetFont();
    T(dl, sf, lpx, ImVec2(nx, ly), hov ? IM_COL32_WHITE : MemberDotCard(m), ms);
    float tx = nx + TW(sf, lpx, ms) + S(10);
    struct { const char* s; ImU32 fg; ImU32 bg; } tags[6];
    int nt = 0;
    if (you)                              tags[nt++] = { "YOU",     IM_COL32_WHITE, IM_COL32(0, 176, 182, 255) };
    if (strcmp(m.cid, sn.ownerCid) == 0)  tags[nt++] = { "OWNER",   kCardInk, kGold };
    if (strcmp(m.cid, sn.leaderCid) == 0) tags[nt++] = { "PICKER",  IM_COL32_WHITE, IM_COL32(0, 141, 213, 255) };
    if (m.nosong)                         tags[nt++] = { "NO SONG", IM_COL32_WHITE, IM_COL32(226, 44, 96, 255) };
    if (m.autoplay)                       tags[nt++] = { "AUTO",    IM_COL32_WHITE, gPinkTag };
    for (int i = 0; i < nt; i++) {
        if (tx + TxtW(tags[i].s, 0.78f) + S(20) > xr) break;
        tx += Tag(dl, ImVec2(tx, ly + S(1)), S(17), tags[i].s, tags[i].fg, tags[i].bg) + S(5);
    }
    return clk;
}

/* what the room is doing right now (the help band's idle text) */
static ImU32 StatusText(const Snapshot& sn, char* foot, int n) {
    ImU32 col = IM_COL32_WHITE;
    if (!sn.code[0]) {
        if (s_joinNote[0]) {
            snprintf(foot, n, "%s", s_joinNote);
            return s_joinNoteCol;
        }
        MpStatus mst;
        mp::GetStatus(&mst);
        if (mst.state == MpState::Connected)
            snprintf(foot, n, "%s  %s  %d ms - create a room or join one with its code",
                     mp::StateName(mst.state), mst.server, mst.rtt_ms);
        else if (mst.last_error[0])
            snprintf(foot, n, "%s  %s  (%s)", mp::StateName(mst.state), mst.server, mst.last_error);
        else
            snprintf(foot, n, "%s  %s", mp::StateName(mst.state), mst.server);
        return col;
    }
    const char* leadName = "";
    for (int i = 0; i < sn.memberCount; i++)
        if (strcmp(sn.members[i].cid, sn.leaderCid) == 0) leadName = sn.members[i].name;
    char noSong[120] = ""; int noSongN = 0;
    for (int i = 0, off = 0; i < sn.memberCount && off < (int)sizeof(noSong) - 44; i++)
        if (sn.members[i].nosong)
            off += snprintf(noSong + off, sizeof(noSong) - off, "%s%s", noSongN++ ? ", " : "",
                            strcmp(sn.members[i].cid, sn.myCid) == 0 ? "You" : sn.members[i].name);
    if (!sn.connected) {
        int left = sn.offlineMs >= 0 ? 30 - sn.offlineMs / 1000 : 30;
        snprintf(foot, n, "Connection lost - reconnecting (leaving the room in %ds)", left < 0 ? 0 : left);
        col = IM_COL32(255, 232, 120, 255);
    } else if (sn.voteKind[0])
        snprintf(foot, n, "%s vote: %d/%d agreed (%ds left)",
                 strcmp(sn.voteKind, "pause") == 0 ? "Pause" : "Resume",
                 sn.voteAgreed, sn.voteNeed, sn.voteEndsIn);
    else if (sn.groupPaused)
        snprintf(foot, n, "GROUP PAUSED");
    else if (noSongN > 0) {
        snprintf(foot, n, "%s %s have this song", noSong, noSongN == 1 ? "doesn't" : "don't");
        col = IM_COL32(255, 196, 224, 255);
    } else if (sn.notReady[0])
        snprintf(foot, n, "Not ready: %s", sn.notReady);
    else if (sn.statusLine[0] && sn.phase != 1)
        snprintf(foot, n, "%s", sn.statusLine);
    else if (strcmp(sn.leaderCid, sn.myCid) == 0)
        snprintf(foot, n, "You're picking the song");
    else
        snprintf(foot, n, "%s is picking the song", leadName[0] ? leadName : "The leader");
    return col;
}

/* what became of the code as Join Room submitted it; empty once the code
 * is edited or the room opened */
static void UpdateJoinNote(const Snapshot& sn) {
    static int s_prevJoin = JOIN_IDLE;
    if (sn.joinState == JOIN_FAILED && s_prevJoin == JOIN_PENDING && s_joinShown)
        PanelSE(1);                       /* the server refused the code */
    s_prevJoin = sn.joinState;
    s_joinNote[0] = 0;
    s_joinBad = false;
    s_joinNoteCol = gWarn;
    if (sn.code[0]) s_joinShown = false;  /* in a room: that join is over */
    if (!s_joinShown || strcmp(s_codeBuf, s_codeSent) != 0) return;
    if (sn.joinState == JOIN_PENDING) {
        snprintf(s_joinNote, sizeof(s_joinNote), "Joining room %s...", sn.joinCode);
        s_joinNoteCol = IM_COL32_WHITE;
    } else if (sn.joinState == JOIN_FAILED) {
        snprintf(s_joinNote, sizeof(s_joinNote), "%s", sn.joinErr);
        s_joinBad = true;
    } else if (!sn.connected) {
        snprintf(s_joinNote, sizeof(s_joinNote), "Not connected to a server yet - can't join a room");
    }
}

static const float kRowStep = 90.0f, kRowW = 900.0f, kRowStagger = 20.0f;
struct RowCur { int i; float y; };
static ImVec2 RowPos(RowCur& c) {
    ImVec2 p(X(64.0f + c.i * kRowStagger), c.y);
    c.i++; c.y += S(kRowStep);
    return p;
}

/* ---------------- Room page ---------------- */
static void RoomPage(const Snapshot& sn, RowCur& rc) {
    const float w = S(kRowW);
    if (!s_nameInit) {
        strncpy(s_nameBuf, Config::mpPlayerName.c_str(), sizeof(s_nameBuf) - 1);
        s_nameInit = true;
    }
    const bool nameDirty = s_nameBuf[0] &&
        strncmp(s_nameBuf, Config::mpPlayerName.c_str(), sizeof(s_nameBuf)) != 0;
    if (FieldRow("name", RowPos(rc), w, "Player Name", s_nameBuf, sizeof(s_nameBuf),
                 ImGuiInputTextFlags_EnterReturnsTrue, nullptr, "Save", nameDirty,
                 "The name the other players see") && nameDirty) {
        PanelSE(0);
        CmdSetName(s_nameBuf);
    }
    if (!sn.code[0]) {
        /* rooms live on the server: both rows are off until it is reached */
        const bool online = sn.connected;
        static const char* const kOffHelp = "Not connected to a server yet - see the Settings page";
        if (ActionRow("##create", RowPos(rc), w, "Create Room",
                      online ? "Open a new room and get its 4-letter code" : kOffHelp, online)) {
            PanelSE(0);
            CmdLobbyCreate();
        }
        /* Enter in the field still submits while offline or with the button
         * off: the outcome goes to the help band either way */
        if (FieldRow("code", RowPos(rc), w, "Join Room", s_codeBuf, sizeof(s_codeBuf),
                     ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue,
                     "CODE", "Join", s_codeBuf[0] != 0 && online,
                     s_joinNote[0] ? s_joinNote : online ? "Type a room's 4-letter code to join it" : kOffHelp,
                     300.0f, s_joinBad) &&
            s_codeBuf[0]) {
            PanelSE(online ? 0 : 1);
            CmdLobbyJoin(s_codeBuf, true);
            memcpy(s_codeSent, s_codeBuf, sizeof(s_codeSent));
            s_joinShown = true;
        }
        return;
    }
    const bool owner = strcmp(sn.myCid, sn.ownerCid) == 0 && sn.connected;
    static const char* const modes[] = { "rotate", "owner", "assigned" };
    static const char* const modeNames[] = { "Rotate", "Owner", "Assigned" };
    int mi = strncmp(sn.pickMode, "assigned", 8) == 0 ? 2 : strncmp(sn.pickMode, "owner", 5) == 0 ? 1 : 0;
    const bool hasAssigned = sn.assignedCid[0] && strcmp(sn.assignedCid, "null") != 0;
    if (int d = OptRow("##pick", RowPos(rc), w, "Song Picker", modeNames[mi],
                       owner ? "Who picks the song: everyone in turn, the room owner, or one chosen player"
                             : "Who picks the song (the room owner can change this)", owner)) {
        PanelSE(2);
        mi = (mi + d + 3) % 3;
        if (mi == 2)
            CmdPickMode("assigned", hasAssigned ? sn.assignedCid
                                    : (sn.leaderCid[0] && strcmp(sn.leaderCid, "null") != 0) ? sn.leaderCid
                                    : sn.myCid);
        else
            CmdPickMode(modes[mi], nullptr);
    }
    if (mi == 2) {
        int ai = -1;
        for (int i = 0; i < sn.memberCount; i++)
            if (hasAssigned && strcmp(sn.members[i].cid, sn.assignedCid) == 0) ai = i;
        if (int d = OptRow("##assignee", RowPos(rc), w, "Assigned Player",
                           ai >= 0 ? sn.members[ai].name : "(none)",
                           owner ? "The player who picks every song - or click a player on the right"
                                 : "The player who picks every song", owner && sn.memberCount > 0)) {
            PanelSE(2);
            ai = ((ai < 0 ? 0 : ai) + d + sn.memberCount) % sn.memberCount;
            CmdPickMode("assigned", sn.members[ai].cid);
        }
    }
    if (OptRow("##samediff", RowPos(rc), w, "Difficulty", sn.sameDiff ? "Same" : "Free",
               owner ? "Free: everyone picks their own difficulty. Same: everyone plays the song picker's"
                     : (sn.sameDiff ? "Everyone plays the song picker's difficulty"
                                    : "Everyone picks their own difficulty"), owner)) {
        PanelSE(2);
        CmdSameDiff(!sn.sameDiff);
    }
    if (sn.noFail >= 0 &&
        OptRow("##nofail", RowPos(rc), w, "No Fail Mode", sn.noFail ? "On" : "Off",
               owner ? "On: songs don't end when the life gauge runs out. Off: they do. Applies to every player"
                     : (sn.noFail ? "Songs don't end when the life gauge runs out (set by the room owner)"
                                  : "A song ends when the life gauge runs out (set by the room owner)"),
               owner)) {
        PanelSE(2);
        CmdNoFail(!sn.noFail);
    }
    if (sn.selPv > 0 && sn.selName[0])
        OptRow("##song", RowPos(rc), w, "Selected Song", sn.selName[0] ? sn.selName : "-",
               "The song the picker has selected", false);
    {
        static double s_leaveUntil = 0.0;
        const bool armed = s_confirmLeave && ImGui::GetTime() < s_leaveUntil;
        if (!armed) s_confirmLeave = false;
        if (ActionRow("##leave", RowPos(rc), w, armed ? "Click again to leave" : "Leave Room",
                      "Leave this room")) {
            if (armed) { s_confirmLeave = false; PanelSE(1); CmdLobbyLeave(); }
            else { s_confirmLeave = true; s_leaveUntil = ImGui::GetTime() + 3.0; PanelSE(2); }
        }
    }
}

/* ---------------- Settings page ---------------- */
static void SettingsPage(RowCur& rc) {
    const float w = S(kRowW);
    static const char* const srv[] = { "Off", "Local", "Online" };
    int mode = Config::mpServerMode == "cloud" ? 2 : Config::mpServerMode == "off" ? 0 : 1;
    static char s_srvHelp[160];
    snprintf(s_srvHelp, sizeof(s_srvHelp),
             "Off, a server on this network (LAN search + %s:%d), or the online server",
             Config::mpServerIp.c_str(), Config::mpServerPort);
    if (int d = OptRow("##srv", RowPos(rc), w, "Server", srv[mode], s_srvHelp)) {
        PanelSE(2);
        mode = (mode + d + 3) % 3;
        mp::SetServerMode(mode == 2 ? kServerCloud : mode == 1 ? kServerLocal : kServerOff);
    }
    if (mode == 2) {
        if (!s_urlInit) {
            strncpy(s_urlBuf, Config::mpCloudUrl.c_str(), sizeof(s_urlBuf) - 1);
            s_urlInit = true;
        }
        if (FieldRow("url", RowPos(rc), w, "Online Address", s_urlBuf, sizeof(s_urlBuf),
                     ImGuiInputTextFlags_EnterReturnsTrue, nullptr, "Apply", true,
                     "Address of the online server (host[:port][/path])", 330.0f)) {
            PanelSE(0);
            mp::SetCloudUrl(s_urlBuf);
        }
    }
    static const char* const styles[] = { "Portraits", "Text", "Off" };
    int st = Config::mpNotifyStyle == "text" ? 1 : Config::mpNotifyStyle == "off" ? 2 : 0;
    if (int d = OptRow("##notify", RowPos(rc), w, "Notifications", styles[st],
                       "How other players' events show up during a song")) {
        PanelSE(2);
        st = (st + d + 3) % 3;
        Config::mpNotifyStyle = st == 1 ? "text" : st == 2 ? "off" : "avatars";
        Config::Save();
    }
    if (OptRow("##ravatars", RowPos(rc), w, "Custom Portraits", Config::mpRemoteAvatars ? "ON" : "OFF",
               "Show portraits other players send for modules this game doesn't have")) {
        PanelSE(2);
        Config::mpRemoteAvatars = !Config::mpRemoteAvatars;
        Config::Save();
    }
    if (OptRow("##chip", RowPos(rc), w, "Status Chip", Config::mpShowStatus ? "ON" : "OFF",
               "The small connection status box in the corner")) {
        PanelSE(2);
        Config::mpShowStatus = !Config::mpShowStatus;
        Config::Save();
    }
    if (OptRow("##ap", RowPos(rc), w, "AutoPlay", AutoPlay::g_enabled ? "ON" : "OFF",
               "Let the game hit the notes (shown to the other players)")) {
        PanelSE(2);
        AutoPlay::g_enabled = !AutoPlay::g_enabled;
        ReportAutoPlay(AutoPlay::g_enabled);
    }
    if (AutoPlay::g_enabled) {
        static const char* const kApModes[] = {
            "Custom", "Auto", "Pro", "97", "Streak", "Human", "Clutch", "Tie"
        };
        int am = AutoPlay::g_mode;
        if (am < 0 || am >= AutoPlay::Mode_Count) am = AutoPlay::Mode_Auto;
        if (int d = OptRow("##apmode", RowPos(rc), w, "AutoPlay Mode", kApModes[am],
                           "How AutoPlay plays: perfect, human-like, a fixed hit rate...")) {
            PanelSE(2);
            AutoPlay::g_mode = (am + d + AutoPlay::Mode_Count) % AutoPlay::Mode_Count;
            ReportAutoPlay(true);
        }
        if (AutoPlay::g_mode == AutoPlay::Mode_Custom) {
            char pv[16];
            snprintf(pv, sizeof(pv), "%d%%", AutoPlay::g_hitPct);
            if (int d = OptRow("##appct", RowPos(rc), w, "Hit Rate", pv,
                               "Share of the notes AutoPlay hits (hold Ctrl for steps of 1)")) {
                PanelSE(2);
                int p = AutoPlay::g_hitPct + d * (ImGui::GetIO().KeyCtrl ? 1 : 5);
                AutoPlay::g_hitPct = p < 0 ? 0 : (p > 100 ? 100 : p);
                ReportAutoPlay(true);
            }
        }
    }
}

/* ---------------- main panel ---------------- */
void DrawPanel() {
    static bool s_prevOpen = false;
    bool open = OverlayOpen();
    if (open != s_prevOpen) {
        s_prevOpen = open;
        if (open) {
            s_nameInit = false;
            s_urlInit = false;
            s_confirmLeave = false;
            s_joinShown = false;
        }
    }
    if (!open) return;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 ds = io.DisplaySize;
    /* laid out on a 1920x1080 canvas centred in the window */
    s_sc = ds.x / 1920.0f < ds.y / 1080.0f ? ds.x / 1920.0f : ds.y / 1080.0f;
    s_ox = (ds.x - 1920.0f * s_sc) * 0.5f;
    const float oy = (ds.y - 1080.0f * s_sc) * 0.5f;
    auto Y = [&](float v) { return oy + v * s_sc; };

    Snapshot sn;
    GetSnapshot(&sn);
    UpdateJoinNote(sn);

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ds, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##mppanel", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* bf = BoldF();
        const char* prevHelp = s_help;      /* last frame's hover, for the band */
        s_help = nullptr;

        /* the Options screens' teal backdrop over the game, with a couple
         * of their diagonal light bands */
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ds, IM_COL32(0, 150, 214, 238), IM_COL32(0, 186, 222, 238),
                                    IM_COL32(0, 212, 210, 238), IM_COL32(0, 176, 206, 238));
        dl->AddQuadFilled(ImVec2(X(1180), 0), ImVec2(X(1300), 0), ImVec2(X(2100), Y(800)), ImVec2(X(1980), Y(800)),
                          IM_COL32(255, 255, 255, 22));
        dl->AddQuadFilled(ImVec2(X(1500), 0), ImVec2(X(1540), 0), ImVec2(X(2340), Y(800)), ImVec2(X(2300), Y(800)),
                          IM_COL32(255, 255, 255, 30));
        dl->AddQuadFilled(ImVec2(X(1100), Y(1080)), ImVec2(X(1190), Y(1080)), ImVec2(X(1990), Y(280)),
                          ImVec2(X(1900), Y(280)), IM_COL32(255, 255, 255, 16));

        /* ===== header tab + page tabs ===== */
        const float hy0 = Y(30), hy1 = Y(88);
        const float tabW = S(96) + TW(bf, S(38), "Multiplayer") + S(40);
        dl->AddQuadFilled(ImVec2(0, hy0), ImVec2(X(0) + tabW, hy0), ImVec2(X(0) + tabW - S(26), hy1),
                          ImVec2(0, hy1), gBlue);
        {   /* two-player glyph in a white disc */
            ImVec2 c(X(48), (hy0 + hy1) * 0.5f);
            dl->AddCircleFilled(c, S(19), IM_COL32_WHITE, 24);
            dl->AddCircleFilled(ImVec2(c.x - S(6), c.y - S(5)), S(5), gBlue, 12);
            dl->AddCircleFilled(ImVec2(c.x + S(7), c.y - S(3)), S(4), gBlue, 12);
            dl->AddRectFilled(ImVec2(c.x - S(13), c.y + S(2)), ImVec2(c.x + S(1), c.y + S(11)), gBlue, S(4));
            dl->AddRectFilled(ImVec2(c.x + S(2), c.y + S(3)), ImVec2(c.x + S(13), c.y + S(11)), gBlue, S(4));
        }
        T(dl, bf, S(38), ImVec2(X(84), (hy0 + hy1) * 0.5f - S(21)), IM_COL32_WHITE, "Multiplayer");
        float hx = X(0) + tabW + S(26);
        static const char* const tabs[2] = { "Room", "Settings" };
        for (int i = 0; i < 2; i++) {
            char id[16]; snprintf(id, sizeof(id), "##tab%d", i);
            if (MiniBtn(id, ImVec2(hx, hy0 + S(7)), ImVec2(hx + S(178), hy1 - S(7)), tabs[i],
                        i == s_tab ? 0 : 2, true,
                        i == 0 ? "Your name, the room and its players" : "Server, notifications and AutoPlay") &&
                i != s_tab) {
                s_tab = i;
                PanelSE(2);
            }
            hx += S(178) + S(16);
        }
        if (sn.code[0]) {
            char rb[24];
            snprintf(rb, sizeof(rb), "ROOM %s", sn.code);
            hx += S(22);
            TOut(dl, bf, S(38), ImVec2(hx, (hy0 + hy1) * 0.5f - S(21)), rb, IM_COL32(0, 96, 150, 255));
            hx += TW(bf, S(38), rb) + S(18);
            if (MiniBtn("##copy", ImVec2(hx, hy0 + S(10)), ImVec2(hx + S(116), hy1 - S(10)), "Copy", 2, true,
                        "Copy the room code to the clipboard")) {
                PanelSE(2);
                ImGui::SetClipboardText(sn.code);
            }
            hx += S(116) + S(18);
            if (sn.round > 0) {
                char rd[20];
                snprintf(rd, sizeof(rd), "Round %d", sn.round);
                TOut(dl, bf, S(26), ImVec2(hx, (hy0 + hy1) * 0.5f - S(14)), rd, IM_COL32(0, 96, 150, 255));
            }
        }

        /* ===== body: light slanted sheet with a blue foot strip ===== */
        dl->AddQuadFilled(ImVec2(0, hy1), ImVec2(X(1060), hy1), ImVec2(X(1196), Y(912)), ImVec2(0, Y(912)),
                          gBody0);
        dl->AddQuadFilled(ImVec2(0, Y(600)), ImVec2(X(1145), Y(600)), ImVec2(X(1196), Y(912)), ImVec2(0, Y(912)),
                          IM_COL32(176, 244, 248, 70));
        dl->AddQuadFilled(ImVec2(X(190), Y(912)), ImVec2(X(1196), Y(912)), ImVec2(X(1199), Y(930)),
                          ImVec2(X(190), Y(930)), gBlue);

        RowCur rc = { 0, Y(112) };
        if (s_tab == 0) RoomPage(sn, rc);
        else SettingsPage(rc);

        /* ===== players (right of the sheet) ===== */
        if (sn.code[0]) {
            const bool owner = strcmp(sn.myCid, sn.ownerCid) == 0 && sn.connected;
            const bool assignMode = owner && strcmp(sn.pickMode, "assigned") == 0;
            int order[8];
            SessionOrder(sn, order);
            const float px0 = X(1236), pw = S(652);
            float py = Y(108);
            char ph[24];
            snprintf(ph, sizeof(ph), "Players  %d", sn.memberCount);
            const float phw = TW(bf, S(28), ph) + S(60);
            dl->AddQuadFilled(ImVec2(px0, py), ImVec2(px0 + phw, py), ImVec2(px0 + phw - S(18), py + S(42)),
                              ImVec2(px0, py + S(42)), gBlue);
            T(dl, bf, S(28), ImVec2(px0 + S(20), py + S(5)), IM_COL32_WHITE, ph);
            py += S(54);
            if (sn.memberCount >= 2)
                py = DrawPodium(dl, sn, order, ImVec2(px0, py), ImVec2(px0 + pw, py + S(148))) + S(8);
            /* the rows scroll (wheel over the list, or drag the bar) when
             * they don't all fit above the help band; offsets in 1080p units */
            static float s_plScroll = 0.0f, s_plScrollTo = 0.0f;
            const float lt = py - S(6), lb = Y(930);
            const float viewH = lb - py;
            const float contentH = sn.memberCount * S(75);
            const float maxScroll = contentH > viewH ? (contentH - viewH) / s_sc : 0.0f;
            const ImVec2 mpos = io.MousePos;
            if (maxScroll > 0.0f && io.MouseWheel != 0.0f && mpos.x >= px0 - S(20) && mpos.x <= ds.x &&
                mpos.y >= lt && mpos.y <= lb)
                s_plScrollTo -= io.MouseWheel * 75.0f;
            const float tx0 = px0 + pw + S(10), tx1 = tx0 + S(8);     /* scroll bar */
            const float trackT = py, trackH = lb - py - S(8);
            const float thumbH = maxScroll > 0.0f ? trackH * viewH / contentH : 0.0f;
            bool barHot = false;
            if (maxScroll > 0.0f) {
                ImGui::SetCursorScreenPos(ImVec2(tx0 - S(8), trackT));
                ImGui::InvisibleButton("##plscroll", ImVec2(S(24), trackH));
                barHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
                if (ImGui::IsItemActive())
                    s_plScroll = s_plScrollTo =
                        (mpos.y - trackT - thumbH * 0.5f) / (trackH - thumbH) * maxScroll;
                else if (ImGui::IsItemHovered())
                    s_help = "Drag to scroll the player list";
            }
            if (s_plScrollTo < 0.0f) s_plScrollTo = 0.0f;
            if (s_plScrollTo > maxScroll) s_plScrollTo = maxScroll;
            {
                const float k = io.DeltaTime * 18.0f;
                s_plScroll += (s_plScrollTo - s_plScroll) * (k < 1.0f ? k : 1.0f);
                if (fabsf(s_plScrollTo - s_plScroll) < 0.5f) s_plScroll = s_plScrollTo;
            }
            /* the clip also limits what the pointer can hit */
            ImGui::PushClipRect(ImVec2(px0 - S(12), lt), ImVec2(px0 + pw + S(8), lb), true);
            float ry = py - S(s_plScroll);
            for (int i = 0; i < sn.memberCount; i++, ry += S(75)) {
                const MemberInfo& m = sn.members[order[i]];
                if (ry + S(75) < lt || ry > lb) continue;
                if (PlayerRow(dl, sn, m, i, ImVec2(px0, ry), pw, owner, assignMode && !m.is_leader)) {
                    PanelSE(2);
                    CmdPickMode("assigned", m.cid);
                }
            }
            ImGui::PopClipRect();
            if (maxScroll > 0.0f) {
                const float ty = trackT + (trackH - thumbH) * (s_plScroll / maxScroll);
                dl->AddRectFilled(ImVec2(tx0, trackT), ImVec2(tx1, trackT + trackH), IM_COL32(255, 255, 255, 70),
                                  S(4));
                dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx1, ty + thumbH),
                                  barHot ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 215), S(4));
            }
        }

        /* ===== help band + title tag ===== */
        const float by0 = Y(938), by1 = Y(1002);
        dl->AddRectFilled(ImVec2(0, by0), ImVec2(ds.x, by1), gBand);
        dl->AddQuadFilled(ImVec2(X(1318), Y(930)), ImVec2(ds.x, Y(930)), ImVec2(ds.x, by1),
                          ImVec2(X(1284), by1), gTitleTag);
        TOut(dl, bf, S(62), ImVec2(X(1372), Y(931)), "Multiplayer", IM_COL32(0, 104, 196, 255));
        {
            char st[220];
            ImU32 col = IM_COL32_WHITE;
            const char* text = prevHelp;
            if (!text || !*text) { col = StatusText(sn, st, sizeof(st)); text = st; }
            else if (text == s_joinNote) col = s_joinNoteCol;
            ImFont* hf = FontFor(text);
            char fb[220];
            Fit(hf, S(27), text, X(1260) - X(70), fb, sizeof(fb));
            T(dl, hf, S(27), ImVec2(X(70), (by0 + by1) * 0.5f - S(15)), col, fb);
        }

        /* ===== footer: own status, key hints ===== */
        dl->AddRectFilled(ImVec2(0, by1), ds, gFooter);
        const float fcy = (by1 + ds.y) * 0.5f;
        float rx = ds.x - (s_ox + S(44));
        bool hintBack = false, hintClose = false;
        rx = KeyHint(dl, rx, fcy, "Esc", "Back", "##hintback", "Close the multiplayer menu", &hintBack);
        rx = KeyHint(dl, rx, fcy, "F8", "Close", "##hintclose", "Close the multiplayer menu - F8 opens it again",
                     &hintClose);
        rx = KeyHint(dl, rx, fcy, "Click", "Select");
        if (hintBack || hintClose) SetOverlayOpen(false);
        if (sn.code[0])
            for (int i = 0; i < sn.memberCount; i++)
                if (strcmp(sn.members[i].cid, sn.myCid) == 0) {
                    const char* ms = MemberStatus(sn.members[i]);
                    StatusDot(dl, ImVec2(X(56), fcy), S(7), MemberDot(sn.members[i]));
                    T(dl, bf, S(25), ImVec2(X(74), fcy - S(14)), gKeyText, ms);
                    break;
                }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace mp::lobby
