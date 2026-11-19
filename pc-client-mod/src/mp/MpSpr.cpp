// put_sprite hook (PC). The public entry exe+0x5B49C0 is
// `mov rdx,rcx; jmp 0x1405B8F80`; the body 0x1405B8F80 takes the SprArgs*
// in RDX (the wrapper and the 5 internal direct callers all set rdx) and
// returns the queued SprArgs* (or 0), so hooking the body sees every
// submission. SprArgs is 0x160 bytes; offsets are from MMPlusMods
// MMPlusModsShared/sprite.hpp, verified against the ctor 0x1405B78D0 and
// the queue-index math at the top of the body.
//
// The multiplayer race lead fill is drawn through this queue (not ImGui)
// so it renders UNDER the gauge's BORDER/G/E markers: DrawGaugeRace
// publishes the lead segments once per ImGui frame into a lock-free
// double buffer that the hook reads on the game's sprite thread.
#include "MpSpr.hpp"
#include "../platform/MpLog.hpp"
#include <safetyhook/safetyhook.hpp>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <intrin.h>

namespace mp::spr {

static inline uintptr_t ExeBase() {
    static uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr);
    return b;
}

/* SprArgs field offsets (0x160 bytes total) */
enum {
    A_KIND = 0x00, A_ID = 0x04, A_COLOR = 0x08, A_ATTR = 0x0C,
    A_INDEX = 0x14, A_LAYER = 0x18, A_PRIO = 0x1C,
    A_RES_S = 0x20, A_RES_T = 0x24,
    A_TRANS = 0x34, A_SCALE = 0x40, A_MAT = 0x60,
    A_TEX = 0xA0, A_NVERT = 0x100,
    A_FLAGS = 0x12C, A_SSIZE = 0x130, A_TPOS = 0x13C, A_TSIZE = 0x144,
    A_SIZEOF = 0x160,
};

static SafetyHookInline s_hook;

/* ================== race lead fill (render-thread published) =========== */
struct PubBuf {
    float    myPct;
    int      n;
    float    pct[8];
    uint32_t color[8];        /* game sprite color: u32 BGRA byte order */
};
static PubBuf           s_pub[2];
static std::atomic<int> s_pubIdx{0};

void RaceFillPublish(float myPct, int n, const float* pct,
                     const uint32_t* color) {
    int w = s_pubIdx.load(std::memory_order_relaxed) ^ 1;
    PubBuf& b = s_pub[w];
    b.myPct = myPct;
    b.n = n > 8 ? 8 : n;
    for (int i = 0; i < b.n; i++) {
        b.pct[i] = pct[i];
        b.color[i] = color[i];
    }
    s_pubIdx.store(w, std::memory_order_release);
}

/* submitted through the trampoline so the detour does not re-enter */
static void* SubmitSprite(uint8_t* args) {
    return s_hook.call<void*>(args, args);
}

/* ---- clear-gauge race fill -------------------------------------------
 * Traced during a song (sprtrace): the bottom "energy" clear gauge is an
 * AET group submitted at ix=0/ly=0, all kind NORMAL with the evaluated
 * transform in mat (+0x60; translation in m[3]/m[7], column convention):
 *   pr10  45624 FRAME_BTM / 45623 FRAME_TOP   (frame edges)
 *   pr15  45621 ENE_BASE_BG_FT               m3=16   y=1031 1904x46 bg
 *   pr15  45622 ENE_BASE_FT x2               m3=194/1846 y=1037 826x26,
 *         the second mirrored (m0=-1) -> bar x 194..1846 (0..100%)
 *   pr15  1835  ENE_BASE_CLR                 m0=~96 (w scale) m3=borderX
 *         y=1038, 12x24 gradient strip = the above-"border" zone
 *   pr15  1829/1830 ENE_REACH/ENE_MSK x2     earned-fill mask (m3=-1605)
 *   pr15  1561  ENERGY_CLR_TXT
 *   pr15  29318/29317 ENE_BORDER_LINE_G/_E   G/E marker notches
 *   pr15  8824  COL_CHIP_RED02               border cursor chip
 *   pr16  2935/2934 ENE_BORDER_TXT/_LINE     BORDER label+line
 * Race segments are submitted as SPR_KIND_RECT quads (untextured, so the
 * race colour comes through exactly like put_sprite_rect in the game's
 * sprite.cpp) into the same ix=0/ly=0/pr=15 queue, injected when 29318
 * passes the detour so they land after the fill family but before the
 * G/E lines and the pr16 BORDER marker -> under every marker. */
enum {
    SPR_ENE_FRAME_BTM = 45624,
    SPR_ENE_BASE_FT   = 45622,
    SPR_ENE_BASE_CLR  = 1835,
    SPR_ENE_BORDER_G  = 29318,
    KIND_RECT         = 3,
    ATTR_MATRIX       = 0x08000000,
    FLAG_SPRITE_SIZE  = 0x01,
};
static float   s_barX0  = 194.0f;
static float   s_barW   = 1652.0f;
static float   s_barY0  = 1038.0f;   /* ENE_BASE_CLR top edge  */
static float   s_barH   = 24.0f;     /* ENE_BASE_CLR height    */
static int32_t s_resS   = 14, s_resT = 14;
static bool    s_sawBar = false, s_sawFill = false;

static float ArgsM(const uint8_t* a, int i) {
    float f; memcpy(&f, a + A_MAT + i * 4, 4); return f;
}

/* Called for every queued sprite (after the original ran) to refresh the
 * geometry for this frame. */
static void RaceFillCapture(const uint8_t* a, void* queued) {
    int32_t id; memcpy(&id, a + A_ID, 4);
    if (id == SPR_ENE_FRAME_BTM) {           /* first sprite of the group */
        s_sawBar = s_sawFill = false;
    } else if (id == SPR_ENE_BASE_FT) {
        float m0 = ArgsM(a, 0), m3 = ArgsM(a, 3);
        if (m0 > 0.0f) {
            s_barX0 = m3;
            if (queued) {                    /* natural width -> full bar */
                float w; memcpy(&w, (const uint8_t*)queued + A_SSIZE, 4);
                if (w > 0.0f) s_barW = w * 2.0f;
            }
        } else if (m3 > s_barX0) {
            s_barW = m3 - s_barX0;           /* mirrored right half edge */
        }
        s_sawBar = true;
    } else if (id == SPR_ENE_BASE_CLR) {
        s_barY0 = ArgsM(a, 7);
        float m5 = ArgsM(a, 5);
        if (queued) {
            float h; memcpy(&h, (const uint8_t*)queued + A_SSIZE + 4, 4);
            if (h > 0.0f) s_barH = h * m5;
        }
        memcpy(&s_resS, a + A_RES_S, 4);
        memcpy(&s_resT, a + A_RES_T, 4);
        s_sawFill = true;
    }
}

/* Untextured flat rect, mirroring put_sprite_rect (MMPlusModsShared
 * sprite.cpp): kind RECT + trans/size + colour + SPR_ATTR_MATRIX. */
static void PutRect(float x0, float y0, float x1, float y1, uint32_t bgra) {
    if (x1 - x0 < 0.25f || y1 - y0 < 0.25f) return;
    uint8_t a[A_SIZEOF];
    ((void(__fastcall*)(void*))(ExeBase() + 0x5B78D0))(a); /* SprArgs ctor */
    *(int32_t*)(a + A_KIND)   = KIND_RECT;
    *(int32_t*)(a + A_INDEX)  = 0;
    *(int32_t*)(a + A_LAYER)  = 0;
    *(int32_t*)(a + A_PRIO)   = 15;
    *(int32_t*)(a + A_RES_S)  = s_resS;
    *(int32_t*)(a + A_RES_T)  = s_resT;
    *(float*)(a + A_TRANS)     = x0;
    *(float*)(a + A_TRANS + 4) = y0;
    *(float*)(a + A_TRANS + 8) = 0.0f;
    *(float*)(a + A_SSIZE)     = x1 - x0;
    *(float*)(a + A_SSIZE + 4) = y1 - y0;
    *(uint32_t*)(a + A_FLAGS) |= FLAG_SPRITE_SIZE;
    *(uint32_t*)(a + A_ATTR)  |= ATTR_MATRIX;
    *(uint32_t*)(a + A_COLOR)  = bgra;
    SubmitSprite(a);
#ifdef MP_DEV_TOOLS
    static int s_segN = 0;
    if (s_segN++ < 4)
        WriteLog("[MP] racefill rect %.1f,%.1f-%.1f,%.1f c=%08x\n",
                 x0, y0, x1, y1, bgra);
#endif
}

static uint32_t ShadeBGRA(uint32_t c, float f) {
    uint32_t b = (uint32_t)((c & 0xFF) * f);         if (b > 255) b = 255;
    uint32_t g = (uint32_t)(((c >> 8) & 0xFF) * f);  if (g > 255) g = 255;
    uint32_t r = (uint32_t)(((c >> 16) & 0xFF) * f); if (r > 255) r = 255;
    return (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

/* Runs inside the detour BEFORE the game's own BORDER_LINE_G queues, so
 * our sprites land under every gauge marker in the pr15 list. */
static void RaceFillDraw() {
#ifdef MP_DEV_TOOLS
    static int s_dbgN = 0;
    if (++s_dbgN % 240 == 1) {
        const PubBuf& pb = s_pub[s_pubIdx.load(std::memory_order_acquire)];
        WriteLog("[MP] racefill: sawBar=%d sawFill=%d bar=%.1f+%.1f"
                 " y=%.1f+%.1f rm=%d/%d n=%d my=%.2f p0=%.2f\n",
                 s_sawBar, s_sawFill, s_barX0, s_barW, s_barY0, s_barH,
                 s_resS, s_resT, pb.n, pb.myPct, pb.n > 0 ? pb.pct[0] : -1.0f);
    }
#endif
    if (!s_sawBar || !s_sawFill || s_barW <= 0.0f) return;
    const PubBuf& b = s_pub[s_pubIdx.load(std::memory_order_acquire)];
    if (b.n <= 0) return;
    auto X = [&](float p) {
        p = p < 0 ? 0 : (p > 100 ? 100 : p);
        return s_barX0 + s_barW * p * 0.01f;
    };
    float myx = X(b.myPct);
    float yT = s_barY0, yB = s_barY0 + s_barH;
    for (int i = 0; i < b.n; i++) {          /* farthest first */
        float d = b.pct[i];
        if (d <= b.myPct + 0.05f) continue;
        float x1 = X(d);
        if (x1 <= myx + 0.25f) continue;
        /* 8 interpolated strips approximating the avatar hexagon's
         * 1.22 -> 0.68 vertical shade ramp */
        for (int s = 0; s < 8; s++) {
            float f = 1.22f + (0.68f - 1.22f) * (s + 0.5f) / 8.0f;
            float ya = yT + s_barH * s / 8.0f;
            PutRect(myx, ya, x1, ya + s_barH / 8.0f + 0.25f,
                    ShadeBGRA(b.color[i], f));
        }
        /* darker right end cap, like the old ImGui fill */
        uint32_t cap = ShadeBGRA(b.color[i], 0.45f);
        PutRect(x1 - 3.0f, yT, x1, yB, cap);
    }
    /* dark seam where the local player's own fill ends */
    PutRect(myx, yT, myx + 3.0f, yB, 0xAA000000u);
}

/* ============================ detour =================================== */
#ifdef MP_DEV_TOOLS
static void TraceRecord(uintptr_t caller, const void* args, void* queued);
#endif
static void* __fastcall PutSpriteDetour(void* rcx, void* rdx) {
#ifdef MP_DEV_TOOLS
    uintptr_t caller = (uintptr_t)_ReturnAddress();
#endif
    const uint8_t* a = (const uint8_t*)rdx;
    int32_t id, ix, ly, kind;
    memcpy(&kind, a + A_KIND, 4);  memcpy(&id, a + A_ID, 4);
    memcpy(&ix, a + A_INDEX, 4);   memcpy(&ly, a + A_LAYER, 4);
#ifdef MP_DEV_TOOLS
    if (id == SPR_ENE_BORDER_G) {
        static int s_gN = 0;
        if (++s_gN % 240 == 1)
            WriteLog("[MP] racefill: LINE_G seen k=%d ix=%d ly=%d\n",
                     kind, ix, ly);
    }
#endif
    if (kind == 0 && id == SPR_ENE_BORDER_G && ix == 0 && ly == 0)
        RaceFillDraw();
    void* ret = s_hook.call<void*>(rcx, rdx);
#ifdef MP_DEV_TOOLS
    TraceRecord(caller, rdx, ret);
#endif
    if (kind == 0 && ix == 0 && ly == 0)
        RaceFillCapture(a, ret);
    return ret;
}

void Install() {
    s_hook = safetyhook::create_inline((void*)(ExeBase() + 0x5B8F80),
                                       (void*)&PutSpriteDetour);
    WriteLog("[MP] put_sprite hook %s @%p\n",
             s_hook ? "installed" : "FAILED",
             (void*)(ExeBase() + 0x5B8F80));
}

#ifdef MP_DEV_TOOLS
/* ======================= sprtrace (dev only) ===========================
 * {cmd:"sprtrace", n:N} captures the next N submissions into a fixed
 * buffer (no formatting on the game thread), then dumps them to the log. */
struct TraceRec {
    uintptr_t ret;
    int32_t   kind, id, index, layer, prio, resS, resT;
    uint32_t  color, attr, flags;
    float     tx, ty, tz, cx, cy, cz, sx, sy, sz, sw, sh, tpx, tpy, tsw, tsh;
    float     m[16];                   /* args+0x60 mat4 */
    uintptr_t tex, next;
    int64_t   nvert;
    float     pvg[3];                  /* PVGameData +2D304/+2D308/+2D30C */
    char      name[36];
    int32_t   qid;                     /* queued args (-1 if none) */
    int32_t   qkind, qindex, qlayer, qprio;
    uint32_t  qinfo, qflags;
    uintptr_t qtex, qnext;
    float     qsw, qsh, qtpx, qtpy, qtsw, qtsh;
};
static TraceRec        s_tr[3072];
static std::atomic<int> s_trPut{0};
static int              s_trCap = 0;
static std::atomic<int> s_trArmed{0};

/* spr_db_spr lookup used by put_sprite for kind NORMAL: +0 id,
 * +8 prj::string_range name, +0x18 spr_info. Read-only map walk - safe to
 * call inside the hook. */
static const char* SprName(int32_t id, char* buf, size_t bufsz) {
    buf[0] = 0;
    __try {
        const uint8_t* e = ((const uint8_t* (__fastcall*)(int32_t))
                            (ExeBase() + 0x5BC870))(id);
        if (!e) return buf;
        const char* p = *(const char**)(e + 0x10);
        const char* e2 = *(const char**)(e + 0x18);
        if ((uintptr_t)p < 0x10000) return buf;
        size_t n = (e2 > p) ? (size_t)(e2 - p) : 0;
        if (!n || n > 200) {
            n = 0;
            while (n < 200 && p[n] >= 0x20 && p[n] < 0x7f) n++;
            if (!n) return buf;
        }
        if (n >= bufsz) n = bufsz - 1;
        memcpy(buf, p, n); buf[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return buf;
}

void TraceArm(int n) {
    if (n <= 0) n = 1500;
    if (n > (int)(sizeof(s_tr) / sizeof(s_tr[0])))
        n = (int)(sizeof(s_tr) / sizeof(s_tr[0]));
    s_trCap = n;
    s_trPut.store(0, std::memory_order_release);
    s_trArmed.store(1, std::memory_order_release);
    WriteLog("[MP] sprtrace armed for %d calls\n", n);
}

static float RdF(const uint8_t* a, int off) {
    float f; memcpy(&f, a + off, 4); return f;
}
static int32_t RdI(const uint8_t* a, int off) {
    int32_t v; memcpy(&v, a + off, 4); return v;
}
static uint32_t RdU(const uint8_t* a, int off) {
    uint32_t v; memcpy(&v, a + off, 4); return v;
}

static void TraceDump() {
    int n = s_trPut.load();
    if (n > s_trCap) n = s_trCap;
    WriteLog("[MP] sprtrace dump %d records\n", n);
    for (int i = 0; i < n; i++) {
        const TraceRec& r = s_tr[i];
        WriteLog("[MP] spr %4d k=%d id=%d ix=%d ly=%d pr=%d rm=%d/%d"
                 " tr=(%.1f,%.1f,%.1f) ctr=(%.1f,%.1f,%.1f) sc=(%.2f,%.2f,%.2f) sz=(%.1f,%.1f)"
                 " tp=(%.1f,%.1f) ts=(%.1f,%.1f) fl=%x c=%08x at=%08x"
                 " tex=%p nx=%p nv=%lld ret=+0x%llx %s\n",
                 i, r.kind, r.id, r.index, r.layer, r.prio, r.resS, r.resT,
                 r.tx, r.ty, r.tz, r.cx, r.cy, r.cz, r.sx, r.sy, r.sz, r.sw, r.sh,
                 r.tpx, r.tpy, r.tsw, r.tsh, r.flags, r.color, r.attr,
                 (void*)r.tex, (void*)r.next,
                 (long long)r.nvert,
                 (unsigned long long)(r.ret - ExeBase()), r.name);
        if (r.pvg[0] != -1.0f)
            WriteLog("[MP]   pvg %.3f %.3f %.3f\n",
                     r.pvg[0], r.pvg[1], r.pvg[2]);
        WriteLog("[MP]   m=(%.3f,%.3f,%.3f,%.3f | %.3f,%.3f,%.3f,%.3f |"
                 " %.3f,%.3f,%.3f,%.3f | %.3f,%.3f,%.3f,%.3f)\n",
                 r.m[0], r.m[1], r.m[2], r.m[3],
                 r.m[4], r.m[5], r.m[6], r.m[7],
                 r.m[8], r.m[9], r.m[10], r.m[11],
                 r.m[12], r.m[13], r.m[14], r.m[15]);
        if (r.qid >= 0)
            WriteLog("[MP]   q k=%d info=%08x ix=%d ly=%d pr=%d fl=%x"
                     " sz=(%.1f,%.1f) tp=(%.1f,%.1f) ts=(%.1f,%.1f)"
                     " tex=%p nx=%p\n",
                     r.qkind, r.qinfo, r.qindex, r.qlayer, r.qprio, r.qflags,
                     r.qsw, r.qsh, r.qtpx, r.qtpy, r.qtsw, r.qtsh,
                     (void*)r.qtex, (void*)r.qnext);
    }
    WriteLog("[MP] sprtrace done\n");
}

static void TraceRecord(uintptr_t caller, const void* argsV, void* queued) {
    (void)queued;
    if (!s_trArmed.load(std::memory_order_acquire)) return;
    int i = s_trPut.fetch_add(1, std::memory_order_acq_rel);
    if (i >= s_trCap) {
        if (i == s_trCap) s_trArmed.store(0, std::memory_order_release);
        return;
    }
    const uint8_t* a = (const uint8_t*)argsV;
    TraceRec& r = s_tr[i];
    r.ret = caller;
    r.kind = RdI(a, A_KIND);  r.id = RdI(a, A_ID);
    r.index = RdI(a, A_INDEX); r.layer = RdI(a, A_LAYER); r.prio = RdI(a, A_PRIO);
    r.resS = RdI(a, A_RES_S); r.resT = RdI(a, A_RES_T);
    r.tx = RdF(a, A_TRANS);   r.ty = RdF(a, A_TRANS + 4);
    r.tz = RdF(a, A_TRANS + 8);
    r.cx = RdF(a, 0x28);      r.cy = RdF(a, 0x2C);
    r.cz = RdF(a, 0x30);
    r.sx = RdF(a, A_SCALE);   r.sy = RdF(a, A_SCALE + 4);
    r.sz = RdF(a, A_SCALE + 8);
    r.sw = RdF(a, A_SSIZE);   r.sh = RdF(a, A_SSIZE + 4);
    r.tpx = RdF(a, A_TPOS);   r.tpy = RdF(a, A_TPOS + 4);
    r.tsw = RdF(a, A_TSIZE);  r.tsh = RdF(a, A_TSIZE + 4);
    memcpy(r.m, a + A_MAT, sizeof(r.m));
    r.color = RdU(a, A_COLOR); r.attr = RdU(a, A_ATTR);
    r.flags = RdU(a, A_FLAGS);
    memcpy(&r.tex, a + A_TEX, 8);
    memcpy(&r.next, a + 0x148, 8);
    int64_t nv; memcpy(&nv, a + A_NVERT, 8); r.nvert = nv;
    r.pvg[0] = r.pvg[1] = r.pvg[2] = -1.0f;
    if (r.index == 0 && r.prio >= 14 && r.prio <= 16) {
        const uint8_t* pvg = (const uint8_t*)(ExeBase() + 0x14C2330 + 0x2D304);
        __try {
            r.pvg[0] = RdF(pvg, 0); r.pvg[1] = RdF(pvg, 4); r.pvg[2] = RdF(pvg, 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    r.qid = -1;
    if (queued) {
        const uint8_t* q = (const uint8_t*)queued;
        __try {
            r.qid    = RdI(q, A_ID);
            r.qinfo  = RdU(q, A_ID);
            r.qkind  = RdI(q, A_KIND);
            r.qindex = RdI(q, A_INDEX);
            r.qlayer = RdI(q, A_LAYER);
            r.qprio  = RdI(q, A_PRIO);
            r.qflags = RdU(q, A_FLAGS);
            r.qsw = RdF(q, A_SSIZE);  r.qsh = RdF(q, A_SSIZE + 4);
            r.qtpx = RdF(q, A_TPOS);  r.qtpy = RdF(q, A_TPOS + 4);
            r.qtsw = RdF(q, A_TSIZE); r.qtsh = RdF(q, A_TSIZE + 4);
            memcpy(&r.qtex, q + A_TEX, 8);
            memcpy(&r.qnext, q + 0x148, 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) { r.qid = -1; }
    }
    r.name[0] = 0;
    if (r.kind == 0 && r.id >= 0) SprName(r.id, r.name, sizeof(r.name));
    if (i + 1 == s_trCap) {
        s_trArmed.store(0, std::memory_order_release);
        TraceDump();
    }
}
#endif

} // namespace mp::spr
