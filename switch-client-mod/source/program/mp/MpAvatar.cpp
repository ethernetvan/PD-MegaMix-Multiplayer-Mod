#include "MpAvatar.hpp"
#include "MpNet.hpp"
#include "../nc/diva_nc.hpp"
#include "../backend/nvn/nvn_Cpp.h"
#include "../backend/imgui_backend/imgui_impl_nvn.hpp"
#include "../lib.hpp"
#include "../logger.hpp"
#include <nn.hpp>
#include <nn/fs.hpp>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mp::avatar {

/* ============================ local module ============================ */

static void* s_mgr = nullptr;

/* shared-set layout inside `mgr` (FUN_000c8ed0): u32 index 0x546c5 =
 * Shared Set A, slot 1 module id; 3 sets x 9 slots x 7 u32, set stride
 * 0x3f u32. */
enum : uint32_t { IDX_SET_A_SLOT1 = 0x546c5 };

static std::atomic<int32_t> s_local{-1};
static std::atomic<bool>    s_announce{false};   /* welcome seen, (re)send */
static int32_t              s_sent = -2;

void CaptureSaveMgr(void* mgr) {
    if (mgr) s_mgr = mgr;
}

int32_t LocalModule() { return s_local.load(std::memory_order_relaxed); }

void OnWelcome() {
    s_sent = -2;
    s_announce.store(true);
}

static uint64_t NowMs() {
    return (uint64_t)nn::os::GetSystemTick().GetInt64Value() * 1000
           / nn::os::GetSystemTickFrequency();
}

static void* ModBase() { return (void*)exl::util::GetMainModuleInfo().m_Total.m_Start; }

/* ============================ portrait cache ============================ */

enum St : uint8_t { ST_FREE, ST_WANTED, ST_LOADING, ST_DRAWN, ST_READY, ST_FAILED, ST_COPYING };

static bool Busy(St s) { return s == ST_LOADING || s == ST_DRAWN || s == ST_COPYING; }

struct Ent {
    int32_t  module = -1;
    St       st = ST_FREE;
    bool     holdsSet = false;   /* our LoadSprSet ref is still held */
    int      setId = -1, sprId = -1, wait = 0;
    void*    src = nullptr;      /* game nvn::Texture*, valid while holdsSet */
    int      rect[4] = {};       /* sprite rect in mip0 px: x, y, w, h */
    int      fmt = -1;
    void*    tex = nullptr;      /* our RGBA ImTextureID */
    int      slot = -1, w = 0, h = 0;
    uint64_t used = 0;
    bool     dump = false;       /* debug: also write the whole card to SD */
};

constexpr int kCache = 10;               /* 8 members + slack; pool has 14 */
static Ent        s_ents[kCache];
static std::mutex s_mtx;
static int        s_unload[kCache];      /* set refs of evicted entries */
static int        s_unloadN = 0;
struct Doomed { int slot; int frames; };
static Doomed     s_doomed[16];
static int        s_doomedN = 0;

static Ent* Find(int32_t module) {
    for (Ent& e : s_ents)
        if (e.st != ST_FREE && e.module == module) return &e;
    return nullptr;
}

static void Release(Ent& e) {
    if (e.tex && s_doomedN < 16) s_doomed[s_doomedN++] = {e.slot, 4};  /* frames in flight */
    if (e.holdsSet && s_unloadN < kCache) s_unload[s_unloadN++] = e.setId;
    e = Ent{};
}

/* free entry, else the least recently used idle one (unused for 3 s) */
static Ent* Alloc(uint64_t now) {
    Ent* best = nullptr;
    for (Ent& e : s_ents) {
        if (e.st == ST_FREE) return &e;
        if (Busy(e.st) || now - e.used < 3000) continue;
        if (!best || e.used < best->used) best = &e;
    }
    if (best) Release(*best);
    return best;
}

int Get(int32_t module, void** tex, float uv0[2], float uv1[2]) {
    if (module < 0 || module > 999) return -1;
    std::lock_guard<std::mutex> g(s_mtx);
    uint64_t now = NowMs();
    Ent* e = Find(module);
    if (!e) {
        if (!(e = Alloc(now))) return 0;
        e->module = module;
        e->st = ST_WANTED;
        WriteLog("[MP-AV] module %d: wanted\n", module);
    }
    e->used = now;
    if (e->st == ST_FAILED) return -1;
    if (e->st != ST_READY) return 0;
    *tex = e->tex;
    uv0[0] = 0.0f; uv0[1] = 0.0f;
    uv1[0] = 1.0f; uv1[1] = 1.0f;
    return 1;
}

void Prefetch(int32_t module) {
    void* t = nullptr; float a[2], b[2];
    Get(module, &t, a, b);
}

/* ============================ game thread ============================ */

struct Sv { const char* a; const char* b; };
static FUNCTION_PTR(long, __fastcall, SetByName,    0x61d910, void* db, Sv* sv);
static FUNCTION_PTR(long, __fastcall, SpriteByName, 0x61db40, void* db, Sv* sv);
#define OFF_SPR_DB 0xcdf360   /* DAT_00cdf360: sprite database object */

/* The game-side steps below run WITHOUT s_mtx: set loads/unloads and
 * DrawSprite can wait on the render thread, which takes s_mtx in Get().
 * A LOADING entry is only touched by the game thread (Busy entries are
 * never evicted); each step returns the next state, committed under the
 * lock. */
static St StartLoad(Ent& e) {
    void* db = *(void**)((char*)ModBase() + OFF_SPR_DB);
    if (!db) return ST_WANTED;                          /* retry next frame */
    char setName[32], sprName[40];
    snprintf(setName, sizeof(setName), "SPR_SEL_MD%03dCMN", e.module);
    snprintf(sprName, sizeof(sprName), "SPR_SEL_MD%03dCMN_MD_IMG", e.module);
    Sv sv1{setName, setName + strlen(setName)};
    Sv sv2{sprName, sprName + strlen(sprName)};
    e.setId = *(int*)SetByName(db, &sv1);
    e.sprId = *(int*)SpriteByName(db, &sv2);
    WriteLog("[MP-AV] module %d: set %d spr %d\n", e.module, e.setId, e.sprId);
    if (e.setId < 0 || e.sprId < 0) return ST_FAILED;
    Sv empty{nullptr, nullptr};
    spr::LoadSprSet((uint32_t)e.setId, (prj::string_view*)&empty);
    e.holdsSet = true;
    e.wait = 0;
    return ST_LOADING;
}

static St FinishLoad(Ent& e) {
    if (spr::CheckSprSetLoading((uint32_t)e.setId)) {
        if (++e.wait <= 600) return ST_LOADING;
        WriteLog("[MP-AV] module %d: set load timeout\n", e.module);
        return ST_FAILED;
    }
    /* draw once at zero alpha: resolves the sprite's texture + px rect */
    SprArgs a;
    a.id = (uint32_t)e.sprId;
    a.trans.x = 0.0f; a.trans.y = 0.0f; a.trans.z = 0.0f;
    a.scale.x = 1.0f; a.scale.y = 1.0f; a.scale.z = 1.0f;
    a.resolution_mode_screen = 13; a.resolution_mode_sprite = 13;
    a.index = 0; a.layer = 0; a.priority = 12;
    a.attr = 0x400000;
    a.color[0] = 0xFF; a.color[1] = 0xFF; a.color[2] = 0xFF; a.color[3] = 0;
    SprArgs* r = spr::DrawSprite(&a);
    /* game texture object: nvn::Texture embedded at +0x28, self-pointer at +0x20 */
    char* obj = r ? (char*)r->tex : nullptr;
    if (!obj || *(char**)(obj + 0x20) != obj + 0x28 ||
        r->texture_size.x < 8 || r->texture_size.y < 8) {
        WriteLog("[MP-AV] module %d: no texture (obj %p)\n", e.module, obj);
        return ST_FAILED;
    }
    e.src = obj + 0x28;
    e.rect[0] = (int)r->texture_pos.x;  e.rect[1] = (int)r->texture_pos.y;
    e.rect[2] = (int)r->texture_size.x; e.rect[3] = (int)r->texture_size.y;
    WriteLog("[MP-AV] module %d: drawn, rect %d,%d %dx%d\n", e.module,
             e.rect[0], e.rect[1], e.rect[2], e.rect[3]);
    return ST_DRAWN;
}

void GamePump() {
    static int s_poll = 0;
    if (s_mgr && (s_poll++ % 20) == 0) {
        int32_t m = ((int32_t*)s_mgr)[IDX_SET_A_SLOT1];
        if (m >= 0 && m <= 99999 && s_local.exchange(m) != m)
            WriteLog("[MP-AV] local module -> %d (mgr %p)\n", m, s_mgr);
    }

    int unload[kCache * 2], n = 0;
    Ent* job = nullptr;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> g(s_mtx);
        for (int i = 0; i < s_unloadN; i++) unload[n++] = s_unload[i];
        s_unloadN = 0;
        for (Ent& e : s_ents)
            if (e.holdsSet && (e.st == ST_READY || e.st == ST_FAILED)) {
                unload[n++] = e.setId;
                e.holdsSet = false;
                e.src = nullptr;
            }
        /* one portrait in flight at a time; newest request first */
        for (Ent& e : s_ents) {
            if (Busy(e.st)) { job = e.st == ST_LOADING ? &e : nullptr; fresh = false; break; }
            if (e.st == ST_WANTED && (!job || e.used > job->used)) { job = &e; fresh = true; }
        }
        if (job && fresh) job->st = ST_LOADING;     /* claim it */
    }
    for (int i = 0; i < n; i++) spr::UnloadSprSet((uint32_t)unload[i]);
    if (!job) return;
    St next = fresh ? StartLoad(*job) : FinishLoad(*job);
    std::lock_guard<std::mutex> g(s_mtx);
    job->st = next;
}

/* ============================ present thread ============================ */

static void Bc4Block(const uint8_t* b, uint8_t out[16]) {
    int p[8] = {b[0], b[1]};
    if (p[0] > p[1]) {
        for (int i = 1; i < 7; i++) p[i + 1] = ((7 - i) * p[0] + i * p[1] + 3) / 7;
    } else {
        for (int i = 1; i < 5; i++) p[i + 1] = ((5 - i) * p[0] + i * p[1] + 2) / 5;
        p[6] = 0; p[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)p[(bits >> (3 * i)) & 7];
}

/* texels [x0,x0+w) x [y0,y0+h) of a BC5 level -> interleaved 2-channel plane */
static void Bc5Region(const uint8_t* lvl, int lw, int lh, int x0, int y0, int w, int h, uint8_t* out) {
    const int bw = (lw + 3) / 4, bh = (lh + 3) / 4;
    for (int by = y0 / 4; by <= (y0 + h - 1) / 4 && by < bh; by++)
        for (int bx = x0 / 4; bx <= (x0 + w - 1) / 4 && bx < bw; bx++) {
            const uint8_t* blk = lvl + ((size_t)by * bw + bx) * 16;
            uint8_t c0[16], c1[16];
            Bc4Block(blk, c0);
            Bc4Block(blk + 8, c1);
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3) - x0, y = by * 4 + (i >> 2) - y0;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                out[((size_t)y * w + x) * 2]     = c0[i];
                out[((size_t)y * w + x) * 2 + 1] = c1[i];
            }
        }
}

static size_t Bc5Bytes(int w, int h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16; }
static float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/* DIVA YCbCr BC5: mip0 = (Y, A), mip1 = (Cb, Cr) at half res. Box-filters
 * the crop [cx,cy,cw,ch] (mip0 px, memory row order) into ow x oh
 * straight-alpha RGBA8, flipped upright (DIVA textures are stored bottom-up). */
static bool DecodeYCbCr(const uint8_t* m0, int w0, int h0, const uint8_t* m1, int w1, int h1,
                        int cx, int cy, int cw, int ch, int ow, int oh, uint8_t* rgba) {
    const int ux0 = cx / 2, uy0 = cy / 2;
    const int uw = (cx + cw + 1) / 2 - ux0, uh = (cy + ch + 1) / 2 - uy0;
    uint8_t* ya = (uint8_t*)malloc((size_t)cw * ch * 2);
    uint8_t* cc = (uint8_t*)malloc((size_t)uw * uh * 2);
    if (!ya || !cc) { free(ya); free(cc); return false; }
    Bc5Region(m0, w0, h0, cx, cy, cw, ch, ya);
    Bc5Region(m1, w1, h1, ux0, uy0, uw, uh, cc);
    for (int oy = 0; oy < oh; oy++) {
        int sy0 = oy * ch / oh, sy1 = (oy + 1) * ch / oh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int ox = 0; ox < ow; ox++) {
            int sx0 = ox * cw / ow, sx1 = (ox + 1) * cw / ow;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            float r = 0, g = 0, b = 0, a = 0; int n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++, n++) {
                    const uint8_t* p = ya + ((size_t)sy * cw + sx) * 2;
                    int qx = (cx + sx) / 2 - ux0, qy = (cy + sy) / 2 - uy0;
                    if (qx >= uw) qx = uw - 1;
                    if (qy >= uh) qy = uh - 1;
                    const uint8_t* q = cc + ((size_t)qy * uw + qx) * 2;
                    float Y = p[0] / 255.0f, A = p[1] / 255.0f;
                    float cb = q[0] / 255.0f * 1.003922f - 0.503929f;
                    float cr = q[1] / 255.0f * 1.003922f - 0.503929f;
                    r += Clamp01(Y + 1.5748f * cr) * A;
                    g += Clamp01(Y - 0.1873f * cb - 0.4681f * cr) * A;
                    b += Clamp01(Y + 1.8556f * cb) * A;
                    a += A;
                }
            uint8_t* o = rgba + ((size_t)(oh - 1 - oy) * ow + ox) * 4;
            float inv = a > 0.0f ? 255.0f / a : 0.0f;
            o[0] = (uint8_t)(r * inv + 0.5f);
            o[1] = (uint8_t)(g * inv + 0.5f);
            o[2] = (uint8_t)(b * inv + 0.5f);
            o[3] = (uint8_t)(a * 255.0f / n + 0.5f);
        }
    }
    free(ya);
    free(cc);
    return true;
}

/* The module image is an upright full-figure card with the figure centred
 * but its top (height, hair, hats) varying by ~30 px. The avatar is a square
 * head-and-shoulders crop, horizontally centred: side = kCropSide x card
 * width, top = kCropHeadroom x side above the figure's first opaque row
 * (tuned on 14 module cards so heads clear the hexagon's top point). */
constexpr float kCropSide     = 0.37f;
constexpr float kCropHeadroom = 0.15f;
constexpr int   kOutSide      = 192;    /* decoded avatar, px (max) */

/* first upright row of the card with >= 3 opaque texels in a central strip */
static int OpaqueTop(const uint8_t* m0, int w0, int h0, int cx, int cy, int cw, int ch) {
    const int sw = cw < 160 ? cw : 160, sx = cx + (cw - sw) / 2;
    uint8_t* ya = (uint8_t*)malloc((size_t)sw * ch * 2);
    if (!ya) return 0;
    Bc5Region(m0, w0, h0, sx, cy, sw, ch, ya);
    int top = 0;
    for (int r = 0; r < ch; r++) {
        const uint8_t* row = ya + (size_t)(ch - 1 - r) * sw * 2;   /* stored bottom-up */
        int n = 0;
        for (int x = 0; x < sw; x++) n += row[x * 2 + 1] > 128;
        if (n >= 3) { top = r; break; }
    }
    free(ya);
    return top;
}

/* the one readback in flight (present thread only) */
static struct { int w[2], h[2]; size_t s[2]; uint8_t* raw; } s_rb;

/* DRAWN -> COPYING: validate the live game texture, queue the GPU copy */
static bool BeginUpload(Ent& e) {
    auto* t = (const nvn::Texture*)e.src;
    e.fmt = (int)t->GetFormat();
    const int levels = t->GetLevels(), w0 = t->GetWidth(), h0 = t->GetHeight();
    WriteLog("[MP-AV] module %d: tex fmt %d levels %d %dx%d\n", e.module, e.fmt, levels, w0, h0);
    if (t->GetFormat() != nvn::Format::RGTC2_UNORM || levels < 2) return false;
    const int cx = e.rect[0], cy = e.rect[1], cw = e.rect[2], ch = e.rect[3];
    if (cx < 0 || cy < 0 || cx + cw > w0 || cy + ch > h0) return false;
    s_rb.w[0] = w0; s_rb.h[0] = h0;
    s_rb.w[1] = w0 > 1 ? w0 / 2 : 1; s_rb.h[1] = h0 > 1 ? h0 / 2 : 1;
    for (int i = 0; i < 2; i++) s_rb.s[i] = Bc5Bytes(s_rb.w[i], s_rb.h[i]);
    s_rb.raw = (uint8_t*)malloc(s_rb.s[0] + s_rb.s[1]);
    if (s_rb.raw && ImguiNvnBackend::BeginTextureReadback(t, 2, s_rb.w, s_rb.h, s_rb.s)) return true;
    free(s_rb.raw);
    s_rb.raw = nullptr;
    return false;
}

/* debug: the whole upright card as raw RGBA8, ExlSD:/av_dump_MMM_WxH.rgba */
static void DumpCard(const Ent& e, const uint8_t* raw) {
    const int w = e.rect[2], h = e.rect[3];
    const size_t n = (size_t)w * h * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (px && DecodeYCbCr(raw, s_rb.w[0], s_rb.h[0], raw + s_rb.s[0], s_rb.w[1], s_rb.h[1],
                          e.rect[0], e.rect[1], w, h, w, h, px)) {
        char path[64];
        snprintf(path, sizeof(path), "ExlSD:/av_dump_%03d_%dx%d.rgba", e.module, w, h);
        nn::fs::DeleteFile(path);
        nn::fs::FileHandle f;
        if (R_SUCCEEDED(nn::fs::CreateFile(path, (int64_t)n)) &&
            R_SUCCEEDED(nn::fs::OpenFile(&f, path, nn::fs::OpenMode_Write))) {
            nn::fs::WriteFile(f, 0, px, n, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
            nn::fs::CloseFile(f);
            WriteLog("[MP-AV] module %d: dumped %s\n", e.module, path);
        }
    }
    free(px);
}

/* COPYING -> READY/FAILED once the fence has signalled; 0 = keep waiting */
static int FinishUpload(Ent& e) {
    uint8_t* raw = s_rb.raw;
    int r = ImguiNvnBackend::PollTextureReadback(raw, s_rb.s[0] + s_rb.s[1]);
    if (r == 0) return 0;
    s_rb.raw = nullptr;
    if (r < 0) {
        free(raw);
        WriteLog("[MP-AV] module %d: readback failed\n", e.module);
        return -1;
    }
    const int cx = e.rect[0], cy = e.rect[1], cw = e.rect[2], ch = e.rect[3];
    int side = (int)(cw * kCropSide + 0.5f);
    if (side > cw) side = cw;
    if (side > ch) side = ch;
    int top = OpaqueTop(raw, s_rb.w[0], s_rb.h[0], cx, cy, cw, ch) - (int)(side * kCropHeadroom + 0.5f);
    if (top < 0) top = 0;
    if (top > ch - side) top = ch - side;
    /* upright top row `top` is memory row cy + ch - 1 - top */
    const int sx = cx + (cw - side) / 2, sy = cy + ch - top - side;
    const int os = side < kOutSide ? side : kOutSide;
    uint8_t* rgba = (uint8_t*)malloc((size_t)os * os * 4);
    bool ok = rgba && DecodeYCbCr(raw, s_rb.w[0], s_rb.h[0], raw + s_rb.s[0], s_rb.w[1], s_rb.h[1],
                                  sx, sy, side, side, os, os, rgba);
    if (ok) {
        e.tex = ImguiNvnBackend::CreateRgbaTexture(os, os, rgba, &e.slot);
        e.w = os; e.h = os;
        ok = e.tex != nullptr;
    }
    WriteLog("[MP-AV] module %d: decoded %d px crop -> %dx%d %s (slot %d)\n", e.module, side, os, os,
             ok ? "ok" : "FAILED", e.slot);
    if (e.dump) DumpCard(e, raw);
    free(raw);
    free(rgba);
    return ok ? 1 : -1;
}

void RenderPump() {
    if (mp::GetState() != MpState::Connected) {
        s_announce.store(false);
    } else if (s_announce.load()) {
        int32_t m = s_local.load();
        if (m >= 0 && m != s_sent) {
            char line[48];
            snprintf(line, sizeof(line), "{\"t\":\"avatar\",\"module\":%d}", m);
            Enqueue(line);
            s_sent = m;
        }
    }

    Ent* job = nullptr;
    {
        std::lock_guard<std::mutex> g(s_mtx);
        for (int i = 0; i < s_doomedN;) {
            if (--s_doomed[i].frames <= 0) {
                ImguiNvnBackend::DestroyRgbaTexture(s_doomed[i].slot);
                s_doomed[i] = s_doomed[--s_doomedN];
            } else {
                i++;
            }
        }
        for (Ent& e : s_ents)
            if (e.st == ST_DRAWN || e.st == ST_COPYING) { job = &e; break; }
    }
    if (!job) return;
    /* DRAWN/COPYING entries are only touched here (no eviction, no
     * game-thread work), so this runs without holding the lock. */
    St next = job->st;
    if (job->st == ST_DRAWN) {
        next = BeginUpload(*job) ? ST_COPYING : ST_FAILED;
    } else {
        int r = FinishUpload(*job);
        if (r != 0) next = r > 0 ? ST_READY : ST_FAILED;
    }
    std::lock_guard<std::mutex> g(s_mtx);
    job->st = next;
}

/* ============================ debug ============================ */

void DbgPoke(int32_t module) {
    WriteLog("[MP-AV] poke %d (mgr %p, was %d)\n", module, s_mgr,
             s_mgr ? ((int32_t*)s_mgr)[IDX_SET_A_SLOT1] : -9);
    if (s_mgr && module >= 0) ((int32_t*)s_mgr)[IDX_SET_A_SLOT1] = module;
}

void DbgDump(int32_t module) {
    if (module < 0 || module > 999) return;
    std::lock_guard<std::mutex> g(s_mtx);
    Ent* e = Find(module);
    if (e && Busy(e->st)) { e->dump = true; return; }
    if (e) Release(*e);                  /* re-decode so the dump happens */
    uint64_t now = NowMs();
    if (!(e = Alloc(now))) return;
    e->module = module;
    e->st = ST_WANTED;
    e->dump = true;
    e->used = now;
}

void DbgPeek() {
    char line[512];
    int off = snprintf(line, sizeof(line), "{\"t\":\"dbg_avatar\",\"local\":%d,\"sent\":%d,\"ents\":[",
                       LocalModule(), s_sent);
    std::lock_guard<std::mutex> g(s_mtx);
    bool first = true;
    for (const Ent& e : s_ents) {
        if (e.st == ST_FREE || off >= (int)sizeof(line) - 48) continue;
        off += snprintf(line + off, sizeof(line) - off, "%s[%d,%d,%d,%d,%d]",
                        first ? "" : ",", e.module, (int)e.st, e.fmt, e.w, e.h);
        first = false;
    }
    snprintf(line + off, sizeof(line) - off, "]}");
    Enqueue(line);
}

}
