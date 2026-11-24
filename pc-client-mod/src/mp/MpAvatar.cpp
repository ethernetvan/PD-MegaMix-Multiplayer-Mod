// PC port of switch-client-mod MpAvatar.cpp: module portraits from the
// game's own SPR_SEL_MDnnnCMN cards. The card sprite set is loaded and
// drawn once at alpha 0 on the game thread; on the present thread the
// BC5 "YCbCr" texture (mip0 = Y/A, mip1 = Cb/Cr) is copied into a D3D11
// staging texture, CPU-decoded like the Switch does, and uploaded as an
// RGBA8 texture whose SRV is the ImTextureID.
#include "MpAvatar.hpp"
#include "MpNet.hpp"
#include "../Stubs.hpp"
#include "../Config.hpp"
#include "../platform/MpLog.hpp"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mp::avatar {

/* ============================ local module ============================ */

static uintptr_t Exe() { return (uintptr_t)GetModuleHandleW(nullptr); }

/* GetSaveData() -> save base; Shared Set A slot 1 u32 at +0x168D68 */
using GetSaveDataFn = void* (*)();
static void* SaveBase() {
    return ((GetSaveDataFn)(Exe() + 0x1D6510))();
}
static int32_t* Slot1() {
    void* b = SaveBase();
    return b ? (int32_t*)((char*)b + 0x168D68) : nullptr;
}

static std::atomic<int32_t> s_local{-1};
static std::atomic<bool>    s_announce{false};   /* welcome seen, (re)send */
static int32_t              s_sent = -2;

/* 96x96 RGBA copy of the LOCAL module's portrait for server upload */
static uint8_t  s_imgRgba[96 * 96 * 4];
static int32_t  s_imgW = 0, s_imgH = 0;
static int32_t  s_imgModule = -1;
static int32_t  s_imgSent = -1;   /* module whose image was uploaded */
static int32_t  s_imgAsked = -1;  /* module whose decode was requested */

void CaptureSaveMgr(void*) {}

int32_t LocalModule() { return s_local.load(std::memory_order_relaxed); }

void OnWelcome() {
    s_sent = -2;
    s_imgSent = -1;   /* server may have restarted (it dedupes anyway) */
    s_announce.store(true);
}

static uint64_t NowMs() { return GetTickCount64(); }

static int B64(char* out, size_t cap, const uint8_t* p, size_t n);

/* ============================ portrait cache ============================ */

/* local portraits: WANTED -> LOADING -> DRAWN -> COPYING -> READY/FAILED
 * remote (opt-in, another console's upload): RWANT -> REQ (avatar_get
 * sent) -> PIXELS (avatar_px decoded on the net thread) -> UPLOAD ->
 * READY/FAILED */
enum St : uint8_t { ST_FREE, ST_WANTED, ST_LOADING, ST_DRAWN, ST_READY, ST_FAILED, ST_COPYING,
                    ST_RWANT, ST_REQ, ST_PIXELS, ST_UPLOAD };

static bool Busy(St s) {
    return s == ST_LOADING || s == ST_DRAWN || s == ST_COPYING || s == ST_UPLOAD;
}

constexpr int kRemotePx = 96 * 96 * 4;   /* avatar_px payload, exact */
constexpr int kMaxRemote = 6;           /* remote portraits cached at once */
constexpr int kRemoteTries = 3;         /* avatar_get attempts per id */

struct Ent {
    int32_t  module = -1;
    char     img[13] = "";       /* remote entry: server portrait id */
    uint8_t* px = nullptr;       /* remote: decoded pixels awaiting upload */
    int      tries = 0;
    uint64_t reqAt = 0;
    St       st = ST_FREE;
    bool     holdsSet = false;   /* our LoadSprSet ref is still held */
    int      setId = -1, sprId = -1, wait = 0;
    void*    src = nullptr;      /* dx_texture*, valid while holdsSet */
    int      rect[4] = {};       /* sprite rect in mip0 px: x, y, w, h */
    int      fmt = -1;
    void*    tex = nullptr;      /* our RGBA ImTextureID (the SRV) */
    int      slot = -1, w = 0, h = 0;
    uint64_t used = 0;
    bool     dump = false;       /* debug: also write the whole card to disk */
};

constexpr int kCache = 10;               /* 8 members + slack */
static Ent        s_ents[kCache];
static std::mutex s_mtx;
static int        s_unload[kCache];      /* set refs of evicted entries */
static int        s_unloadN = 0;
struct Doomed { int slot; int frames; };
static Doomed     s_doomed[16];
static int        s_doomedN = 0;

static Ent* Find(int32_t module) {
    for (Ent& e : s_ents)
        if (e.st != ST_FREE && !e.img[0] && e.module == module) return &e;
    return nullptr;
}

static Ent* FindImg(const char* img) {
    for (Ent& e : s_ents)
        if (e.st != ST_FREE && e.img[0] && strcmp(e.img, img) == 0) return &e;
    return nullptr;
}

/* ============================ D3D11 helpers ============================ */

static ID3D11Device*        s_dev = nullptr;
static ID3D11DeviceContext* s_ctx = nullptr;

void SetDevice(void* dev, void* ctx) {
    s_dev = (ID3D11Device*)dev;
    s_ctx = (ID3D11DeviceContext*)ctx;
}

/* texture slots: an SRV (the ImTextureID) + its texture, freed via slots
 * so a doomed texture can be released a few frames after eviction */
constexpr int kSlots = 24;
static struct { ID3D11ShaderResourceView* srv; ID3D11Texture2D* tex; } s_slots[kSlots];

static void* CreateRgbaTexture(int w, int h, const uint8_t* rgba, int* slot) {
    *slot = -1;
    if (!s_dev) return nullptr;
    int free_ = -1;
    for (int i = 0; i < kSlots; i++)
        if (!s_slots[i].srv) { free_ = i; break; }
    if (free_ < 0) return nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { rgba, (UINT)(w * 4), 0 };
    ID3D11Texture2D* t = nullptr;
    if (FAILED(s_dev->CreateTexture2D(&d, &sd, &t)) || !t) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(s_dev->CreateShaderResourceView(t, nullptr, &srv)) || !srv) {
        t->Release();
        return nullptr;
    }
    s_slots[free_].srv = srv;
    s_slots[free_].tex = t;
    *slot = free_;
    return srv;
}

static void DestroyRgbaTexture(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    if (s_slots[slot].srv) s_slots[slot].srv->Release();
    if (s_slots[slot].tex) s_slots[slot].tex->Release();
    s_slots[slot] = {};
}

static void Release(Ent& e) {
    if (e.tex && s_doomedN < 16) s_doomed[s_doomedN++] = {e.slot, 4};  /* frames in flight */
    if (e.holdsSet && s_unloadN < kCache) s_unload[s_unloadN++] = e.setId;
    free(e.px);
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

static bool ValidImg(const char* s) {
    int n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f'))) return false;
    return n == 12;
}

static int GetRemote(const char* img, void** tex) {
    if (!ValidImg(img)) return -1;
    std::lock_guard<std::mutex> g(s_mtx);
    uint64_t now = NowMs();
    Ent* e = FindImg(img);
    if (!e) {
        int n = 0;
        for (const Ent& x : s_ents) n += x.st != ST_FREE && x.img[0];
        if (n >= kMaxRemote || !(e = Alloc(now))) return n >= kMaxRemote ? -1 : 0;
        memcpy(e->img, img, 13);
        e->st = ST_RWANT;
        WriteLog("[MP-AV] remote %s: wanted\n", img);
    }
    e->used = now;
    if (e->st == ST_FAILED) return -1;
    if (e->st != ST_READY) return 0;
    *tex = e->tex;
    return 1;
}

int Resolve(const Ref& r, void** tex, float uv0[2], float uv1[2]) {
    uv0[0] = uv0[1] = 0.0f;
    uv1[0] = uv1[1] = 1.0f;
    /* no table (not loaded yet): plain module lookup, as before */
    if (!moduledb::Available()) return Get(r.module, tex, uv0, uv1);
    moduledb::Info inf;
    if (r.module >= 0 && moduledb::Find(r.module, &inf) && (r.mh == 0 || inf.sig == r.mh)) {
        int g = Get(r.module, tex, uv0, uv1);
        if (g != -1) return g;
    }
    if (Config::mpRemoteAvatars && r.img[0]) {
        int g = GetRemote(r.img, tex);
        if (g != -1) return g;
    }
    int32_t dm = moduledb::DefaultModuleFor(r.chara);
    return dm >= 0 ? Get(dm, tex, uv0, uv1) : -1;
}

static int B64Val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* exactly kRemotePx bytes (a multiple of 3: no padding allowed) */
static bool DecodeRemote(const char* s, int n, uint8_t* out) {
    if (n != kRemotePx / 3 * 4) return false;
    for (int i = 0, o = 0; i < n; i += 4, o += 3) {
        int a = B64Val(s[i]), b = B64Val(s[i + 1]), c = B64Val(s[i + 2]), d = B64Val(s[i + 3]);
        if ((a | b | c | d) < 0) return false;
        uint32_t v = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)c << 6 | (uint32_t)d;
        out[o] = (uint8_t)(v >> 16); out[o + 1] = (uint8_t)(v >> 8); out[o + 2] = (uint8_t)v;
    }
    return true;
}

void OnRemotePixels(const char* img, const char* b64, int n, bool err) {
    uint8_t* px = nullptr;
    if (!err && Config::mpRemoteAvatars) {
        px = (uint8_t*)malloc(kRemotePx);
        if (px && !DecodeRemote(b64, n, px)) { free(px); px = nullptr; }
    }
    std::lock_guard<std::mutex> g(s_mtx);
    Ent* e = ValidImg(img) ? FindImg(img) : nullptr;
    if (!e || e->st != ST_REQ) { free(px); return; }   /* unsolicited / stale */
    if (px) { e->px = px; e->st = ST_PIXELS; }
    else {
        /* refused (hidden, left the room, relay off) or malformed: final */
        e->st = ST_FAILED;
        WriteLog("[MP-AV] remote %s: %s\n", img, err ? "refused" : "bad payload");
    }
}

void DropRemote() {
    std::lock_guard<std::mutex> g(s_mtx);
    for (Ent& e : s_ents)
        if (e.st != ST_FREE && e.img[0] && !Busy(e.st)) Release(e);
}

/* ============================ game thread ============================ */

/* the module-table entry (Stubs moduledb and this file share the layout):
 * +0x6C SPR_SEL_MDnnnCMN sprite-set id, +0x74 its MD_IMG sprite id */
static bool SpriteIds(int32_t module, int* set, int* spr) {
    const uint8_t* e = moduledb::RawEntry(module);
    if (!e) return false;
    *set = *(const int32_t*)(e + 0x6C);
    *spr = *(const int32_t*)(e + 0x74);
    return *set >= 0 && *spr >= 0;
}

using LoadSetFn = void (__fastcall*)(uint32_t, void*);
using LoadingFn = bool (__fastcall*)(uint32_t);
using SprCtorFn = void (__fastcall*)(void*);
using PutSprFn  = void* (__fastcall*)(void*);

static void LoadSet(uint32_t set) {
    static uint8_t s_strv[32];
    memset(s_strv, 0, sizeof(s_strv));
    ((LoadSetFn)(Exe() + 0x5B4770))(set, s_strv);
}
static bool SetLoading(uint32_t set) {
    return ((LoadingFn)(Exe() + 0x5B4810))(set);
}
static void UnloadSet(uint32_t set) {
    ((void (__fastcall*)(uint32_t))(Exe() + 0x5B48B0))(set);
}

/* The game-side steps below run WITHOUT s_mtx: set loads/unloads and the
 * put_sprite probe can wait on the render thread, which takes s_mtx in
 * Get(). A LOADING entry is only touched by the game thread (Busy
 * entries are never evicted); each step returns the next state,
 * committed under the lock. */
static St StartLoad(Ent& e) {
    if (!SpriteIds(e.module, &e.setId, &e.sprId)) {
        WriteLog("[MP-AV] module %d: no table entry\n", e.module);
        return ST_FAILED;
    }
    WriteLog("[MP-AV] module %d: set %d spr %d\n", e.module, e.setId, e.sprId);
    LoadSet((uint32_t)e.setId);
    e.holdsSet = true;
    e.wait = 0;
    return ST_LOADING;
}

static St FinishLoad(Ent& e) {
    if (SetLoading((uint32_t)e.setId)) {
        if (++e.wait <= 600) return ST_LOADING;
        WriteLog("[MP-AV] module %d: set load timeout\n", e.module);
        return ST_FAILED;
    }
    /* draw once at zero alpha: resolves the sprite's texture + px rect */
    alignas(16) uint8_t a[0x200];
    memset(a, 0, sizeof(a));
    ((SprCtorFn)(Exe() + 0x5B78D0))(a);
    *(uint32_t*)(a + 0x04) = (uint32_t)e.sprId;
    *(int32_t*)(a + 0x20) = 13; *(int32_t*)(a + 0x24) = 13;   /* HD res modes */
    a[8] = a[9] = a[10] = 255; a[11] = 0;                   /* invisible */
    uint8_t* q = (uint8_t*)((PutSprFn)(Exe() + 0x5B49C0))(a);
    /* queued +0xA0 = the engine's dx_texture pointer:
     * +0x10 ID3D11Texture2D*, +0x18 SRV, +0x20 format (14 = BC5),
     * +0x2C/+0x30 w/h, +0x34 mip count */
    char* obj = q ? *(char**)(q + 0xA0) : nullptr;   /* engine texture object */
    char* dx = obj ? *(char**)(obj + 0x28) : nullptr; /* -> dx_texture */
    if (!dx && q) dx = *(char**)(q + 0x158);          /* args' texture ptr */
#ifdef MP_DEV_TOOLS
    if (q) {
        for (int r = 0x90; r < 0x160; r += 16) {
            char line[128]; int o = 0;
            for (int i = 0; i < 16; i++) o += snprintf(line + o, sizeof(line) - o, "%02x", q[r + i]);
            WriteLog("[MP-AV]   q+%03x %s\n", r, line);
        }
        if (dx) {
            const uint8_t* d = (const uint8_t*)dx;
            for (int r = 0; r < 0x40; r += 16) {
                char line[128]; int o = 0;
                for (int i = 0; i < 16; i++) o += snprintf(line + o, sizeof(line) - o, "%02x", d[r + i]);
                WriteLog("[MP-AV]   dx+%02x %s\n", r, line);
            }
        }
    }
#endif
    if (!dx) {
        WriteLog("[MP-AV] module %d: no texture (args %p obj %p)\n",
                 e.module, (void*)q, (void*)obj);
        return ST_FAILED;
    }
    const float tpX = *(float*)(q + 0x13C), tpY = *(float*)(q + 0x140);
    const float tsX = *(float*)(q + 0x144), tsY = *(float*)(q + 0x148);
    const int tw = *(const int32_t*)(dx + 0x2C), th = *(const int32_t*)(dx + 0x30);
    /* texture_pos/size come back in tex px when the sprite knows its
     * source rect; normalized 0..1 otherwise */
    float rx = tpX, ry = tpY, rw = tsX, rh = tsY;
    if (tsX <= 1.01f && tsY <= 1.01f && tsX > 0.0f && tsY > 0.0f) {
        rx *= tw; ry *= th; rw *= tw; rh *= th;
    }
    e.src = dx;
    e.rect[0] = (int)rx; e.rect[1] = (int)ry;
    e.rect[2] = (int)rw; e.rect[3] = (int)rh;
    WriteLog("[MP-AV] module %d: drawn, rect %d,%d %dx%d tex %dx%d fmt %d\n",
             e.module, e.rect[0], e.rect[1], e.rect[2], e.rect[3],
             tw, th, *(const int32_t*)(dx + 0x20));
    if (e.rect[2] < 8 || e.rect[3] < 8) return ST_FAILED;
    return ST_DRAWN;
}

void GamePump() {
    moduledb::MarkReady();
    static int s_poll = 0;
    if ((s_poll++ % 20) == 0) {
        int32_t* s = Slot1();
        int32_t m = s ? *s : -1;
        if (m >= 0 && m <= 99999 && s_local.exchange(m) != m)
            WriteLog("[MP-AV] local module -> %d\n", m);
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
    for (int i = 0; i < n; i++) UnloadSet((uint32_t)unload[i]);
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
/* Head width = mean opaque texels per row 0.075..0.125 x card width below
 * the first opaque row (normal modules 0.12..0.24 x width, Mikudayo 0.45).
 * From kBigHeadFrom (no change) to kBigHeadFull the crop blends to side =
 * kBigHeadSide x head width (capped at kBigHeadMax x width), centred on the
 * head's opaque centroid and kBigHeadFaceY x head width below its top. */
constexpr float kBigHeadFrom  = 0.31f;
constexpr float kBigHeadFull  = 0.45f;
constexpr float kBigHeadSide  = 1.2f;
constexpr float kBigHeadMax   = 0.6f;
constexpr float kBigHeadFaceY = 0.6f;

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

/* mean opaque texels per upright row (*w) and their mean x relative to cx
 * (*x) over rows head + 0.075..0.125 x cw */
static bool HeadBand(const uint8_t* m0, int w0, int h0, int cx, int cy, int cw, int ch,
                     int head, float* w, float* x) {
    const int r0 = head + (int)(cw * 0.075f), r1 = head + (int)(cw * 0.125f);
    if (r1 >= ch || r1 <= r0) return false;
    const int rows = r1 - r0 + 1;
    uint8_t* ya = (uint8_t*)malloc((size_t)cw * rows * 2);
    if (!ya) return false;
    /* upright rows r0..r1 = memory rows cy+ch-1-r1 .. cy+ch-1-r0 */
    Bc5Region(m0, w0, h0, cx, cy + ch - 1 - r1, cw, rows, ya);
    uint64_t n = 0, sx = 0;
    for (int r = 0; r < rows; r++)
        for (int i = 0; i < cw; i++)
            if (ya[((size_t)r * cw + i) * 2 + 1] > 128) { n++; sx += i; }
    free(ya);
    if (!n) return false;
    *w = (float)n / rows;
    *x = (float)sx / n;
    return true;
}

/* the one readback in flight (present thread only) */
static struct {
    int w[2], h[2]; size_t s[2]; uint8_t* raw;
    ID3D11Texture2D* staging;
} s_rb;

/* DRAWN -> COPYING: validate the live game texture, queue the GPU copy */
static bool BeginUpload(Ent& e) {
    const char* dx = (const char*)e.src;
    ID3D11Texture2D* src = dx ? *(ID3D11Texture2D**)(dx + 0x10) : nullptr;
    const int fmt = dx ? *(const int32_t*)(dx + 0x20) : -1;
    const int w0 = dx ? *(const int32_t*)(dx + 0x2C) : 0;
    const int h0 = dx ? *(const int32_t*)(dx + 0x30) : 0;
    const int levels = dx ? *(const int32_t*)(dx + 0x34) : 0;
    WriteLog("[MP-AV] module %d: tex fmt %d levels %d %dx%d\n", e.module, fmt, levels, w0, h0);
    e.fmt = fmt;
    if (!src || fmt != 14 || levels < 2 || !s_dev || !s_ctx) return false;
    const int cx = e.rect[0], cy = e.rect[1], cw = e.rect[2], ch = e.rect[3];
    if (cx < 0 || cy < 0 || cx + cw > w0 || cy + ch > h0) return false;

    D3D11_TEXTURE2D_DESC sd;
    src->GetDesc(&sd);
    if ((int)sd.MipLevels < 2 || sd.Format != DXGI_FORMAT_BC5_UNORM) return false;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* stg = nullptr;
    if (FAILED(s_dev->CreateTexture2D(&sd, nullptr, &stg)) || !stg) return false;
    s_ctx->CopyResource(stg, src);   /* async; Map next frame completes it */

    s_rb.staging = stg;
    s_rb.w[0] = w0; s_rb.h[0] = h0;
    s_rb.w[1] = w0 > 1 ? w0 / 2 : 1; s_rb.h[1] = h0 > 1 ? h0 / 2 : 1;
    for (int i = 0; i < 2; i++) s_rb.s[i] = Bc5Bytes(s_rb.w[i], s_rb.h[i]);
    s_rb.raw = (uint8_t*)malloc(s_rb.s[0] + s_rb.s[1]);
    if (s_rb.raw) return true;
    s_rb.staging->Release();
    s_rb.staging = nullptr;
    return false;
}

/* debug: the whole upright card as raw RGBA8 -> av_dump_MMM_WxH.rgba */
static void DumpCard(const Ent& e, const uint8_t* raw) {
    const int w = e.rect[2], h = e.rect[3];
    const size_t n = (size_t)w * h * 4;
    uint8_t* px = (uint8_t*)malloc(n);
    if (px && DecodeYCbCr(raw, s_rb.w[0], s_rb.h[0], raw + s_rb.s[0], s_rb.w[1], s_rb.h[1],
                          e.rect[0], e.rect[1], w, h, w, h, px)) {
        char name[64];
        snprintf(name, sizeof(name), "av_dump_%03d_%dx%d.rgba", e.module, w, h);
        std::string path = Config::PathA(name);
        FILE* f = fopen(path.c_str(), "wb");
        if (f) {
            fwrite(px, 1, n, f);
            fclose(f);
            WriteLog("[MP-AV] module %d: dumped %s\n", e.module, path.c_str());
        }
    }
    free(px);
}

/* COPYING -> READY/FAILED once the staging copy is mapped */
static int FinishUpload(Ent& e) {
    if (!s_ctx || !s_rb.staging || !s_rb.raw) return -1;
    bool ok = true;
    uint8_t* raw = s_rb.raw;
    for (int i = 0; i < 2 && ok; i++) {
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(s_ctx->Map(s_rb.staging, i, D3D11_MAP_READ, 0, &ms))) { ok = false; break; }
        /* BC5 rows = ceil(w/4)*16 bytes; RowPitch may be padded */
        const size_t rowB = (size_t)((s_rb.w[i] + 3) / 4) * 16;
        const int rows = (s_rb.h[i] + 3) / 4;
        uint8_t* dst = raw + (i ? s_rb.s[0] : 0);
        for (int r = 0; r < rows; r++)
            memcpy(dst + r * rowB, (const uint8_t*)ms.pData + r * ms.RowPitch, rowB);
        s_ctx->Unmap(s_rb.staging, i);
    }
    s_rb.staging->Release();
    s_rb.staging = nullptr;
    s_rb.raw = nullptr;
    if (!ok) {
        free(raw);
        WriteLog("[MP-AV] module %d: readback failed\n", e.module);
        return -1;
    }
    const int cx = e.rect[0], cy = e.rect[1], cw = e.rect[2], ch = e.rect[3];
    int side = (int)(cw * kCropSide + 0.5f);
    if (side > cw) side = cw;
    if (side > ch) side = ch;
    const int head = OpaqueTop(raw, s_rb.w[0], s_rb.h[0], cx, cy, cw, ch);
    int top = head - (int)(side * kCropHeadroom + 0.5f);
    int sx = cx + (cw - side) / 2;
    /* big heads (chibi modules such as Mikudayo) would only show hair:
     * grow the crop with the head and centre it on the face, still
     * smaller than the head so it overflows the frame */
    float hw, hx;
    if (HeadBand(raw, s_rb.w[0], s_rb.h[0], cx, cy, cw, ch, head, &hw, &hx)) {
        float t = (hw - kBigHeadFrom * cw) / ((kBigHeadFull - kBigHeadFrom) * cw);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        if (t > 0.0f) {
            int big = (int)(hw * kBigHeadSide);
            if (big > (int)(cw * kBigHeadMax)) big = (int)(cw * kBigHeadMax);
            if (big > ch) big = ch;
            if (big > side) {
                const int base = side;
                side = base + (int)((big - base) * t + 0.5f);
                const float topBig = head + hw * kBigHeadFaceY - side * 0.5f;
                top = (int)(top + (topBig - top) * t + 0.5f);
                const float sxBig = cx + hx - side * 0.5f;
                const float sxBase = cx + (cw - side) * 0.5f;
                sx = (int)(sxBase + (sxBig - sxBase) * t + 0.5f);
            }
        }
    }
    if (top < 0) top = 0;
    if (top > ch - side) top = ch - side;
    if (sx < cx) sx = cx;
    if (sx > cx + cw - side) sx = cx + cw - side;
    /* upright top row `top` is memory row cy + ch - 1 - top */
    const int sy = cy + ch - top - side;
    const int os = side < kOutSide ? side : kOutSide;
    uint8_t* rgba = (uint8_t*)malloc((size_t)os * os * 4);
    bool dOk = rgba && DecodeYCbCr(raw, s_rb.w[0], s_rb.h[0], raw + s_rb.s[0], s_rb.w[1], s_rb.h[1],
                                  sx, sy, side, side, os, os, rgba);
    if (dOk) {
        e.tex = CreateRgbaTexture(os, os, rgba, &e.slot);
        e.w = os; e.h = os;
        dOk = e.tex != nullptr;
    }
    WriteLog("[MP-AV] module %d: decoded %d px crop -> %dx%d %s (slot %d)\n", e.module, side, os, os,
             dOk ? "ok" : "FAILED", e.slot);
    if (e.dump) DumpCard(e, raw);
    /* keep a 96x96 RGBA copy of the LOCAL module's portrait for the
     * server upload. os <= 96: copy as-is (w = h = os, no upscale).
     * os > 96: alpha-weighted box downscale - dst pixel averages the
     * src block [x*os/96, max(x0+1,(x+1)*os/96)) x same for y; rgb
     * weighted by alpha (0 when sum a == 0), a = mean alpha. */
    if (dOk && e.module == s_local.load(std::memory_order_relaxed)) {
        static const int O = 96;
        if (os <= O) {
            memcpy(s_imgRgba, rgba, (size_t)os * os * 4);
            s_imgW = s_imgH = os;
        } else {
            for (int y = 0; y < O; y++) {
                int y0 = y * os / O, y1 = (y + 1) * os / O;
                if (y1 <= y0) y1 = y0 + 1;
                for (int x = 0; x < O; x++) {
                    int x0 = x * os / O, x1 = (x + 1) * os / O;
                    if (x1 <= x0) x1 = x0 + 1;
                    uint64_t sa = 0, sr = 0, sg = 0, sb = 0;
                    for (int sy = y0; sy < y1; sy++)
                        for (int sx = x0; sx < x1; sx++) {
                            const uint8_t* p = rgba + (sy * os + sx) * 4;
                            sa += p[3]; sr += p[0] * p[3];
                            sg += p[1] * p[3]; sb += p[2] * p[3];
                        }
                    int n = (y1 - y0) * (x1 - x0);
                    uint8_t* d = &s_imgRgba[(y * O + x) * 4];
                    d[0] = (uint8_t)(sa ? sr / sa : 0);
                    d[1] = (uint8_t)(sa ? sg / sa : 0);
                    d[2] = (uint8_t)(sa ? sb / sa : 0);
                    d[3] = (uint8_t)(sa / n);
                }
            }
            s_imgW = s_imgH = O;
        }
        s_imgModule = e.module;
    }
    free(raw);
    free(rgba);
    return dOk ? 1 : -1;
}

/* opt-in remote portraits: one avatar_get in flight at a time (8 s
 * timeout, kRemoteTries attempts); decoded pixels become a texture here */
static void RemotePump(bool connected) {
    static bool s_wasOn = false;
    const bool on = Config::mpRemoteAvatars;
    if (s_wasOn && !on) DropRemote();
    s_wasOn = on;
    const uint64_t now = NowMs();
    Ent* up = nullptr;
    uint8_t* px = nullptr;
    char req[13] = "";
    {
        std::lock_guard<std::mutex> g(s_mtx);
        Ent* want = nullptr;
        bool inFlight = false;
        for (Ent& e : s_ents) {
            if (e.st == ST_FREE || !e.img[0]) continue;
            if (e.st == ST_REQ) {
                if (!connected || now - e.reqAt > 8000)
                    e.st = e.tries >= kRemoteTries ? ST_FAILED : ST_RWANT;
                else
                    inFlight = true;
            }
            if (e.st == ST_RWANT && (!want || e.used > want->used)) want = &e;
            if (e.st == ST_PIXELS && !up) up = &e;
        }
        if (on && connected && want && !inFlight) {
            want->st = ST_REQ;
            want->reqAt = now;
            want->tries++;
            memcpy(req, want->img, sizeof(req));
        }
        if (up) { px = up->px; up->px = nullptr; up->st = ST_UPLOAD; }
    }
    if (req[0]) {
        char line[64];
        snprintf(line, sizeof(line), "{\"t\":\"avatar_get\",\"img\":\"%s\"}", req);
        Enqueue(line);
    }
    if (up) {
        int slot = -1;
        void* tex = CreateRgbaTexture(96, 96, px, &slot);
        free(px);
        std::lock_guard<std::mutex> g(s_mtx);
        up->tex = tex; up->slot = slot; up->w = up->h = 96;
        up->st = tex ? ST_READY : ST_FAILED;
        WriteLog("[MP-AV] remote %s: %s (slot %d)\n", up->img, tex ? "ready" : "no texture", slot);
    }
}

void RenderPump() {
    const bool connected = mp::GetState() == MpState::Connected;
    RemotePump(connected);
    if (!connected) {
        s_announce.store(false);
    } else if (s_announce.load()) {
        int32_t m = s_local.load();
        if (m >= 0 && m != s_sent) {
            /* the module's character + table signature let other consoles
             * notice a module they lack (or that differs on theirs) */
            moduledb::Info inf;
            const bool known = moduledb::Find(m, &inf);
            char line[96];
            snprintf(line, sizeof(line), "{\"t\":\"avatar\",\"module\":%d,\"chara\":%d,\"mh\":%u}",
                     m, known ? inf.chara : -1, known ? inf.sig : 0u);
            Enqueue(line);
            s_sent = m;
        }
        /* upload the local portrait once per module per connection */
        if (m >= 0 && m != s_imgSent) {
            if (s_imgModule == m && s_imgW > 0) {
                static char buf[64 * 1024];
                int n = snprintf(buf, sizeof(buf),
                    "{\"t\":\"avatar_img\",\"module\":%d,\"w\":%d,\"h\":%d,"
                    "\"rgba\":\"", m, s_imgW, s_imgH);
                n += B64(buf + n, sizeof(buf) - n, s_imgRgba,
                         s_imgW * s_imgH * 4);
                n += snprintf(buf + n, sizeof(buf) - n, "\"}");
                if (EnqueueBlob(buf, n)) s_imgSent = m;
            } else if (m != s_imgAsked) {
                /* decode it once (custom/failed: no upload, no retry) */
                std::lock_guard<std::mutex> g(s_mtx);
                Ent* e = Find(m);
                if (e && e->st == ST_READY) Release(*e);
                if (!e || e->st == ST_FREE) {
                    if (!e) e = Alloc(NowMs());
                    if (e) { e->module = m; e->st = ST_WANTED; }
                }
                s_imgAsked = m;
            }
        }
    }

    Ent* job = nullptr;
    {
        std::lock_guard<std::mutex> g(s_mtx);
        for (int i = 0; i < s_doomedN;) {
            if (--s_doomed[i].frames <= 0) {
                DestroyRgbaTexture(s_doomed[i].slot);
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

static int B64(char* out, size_t cap, const uint8_t* p, size_t n) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0, i;
    for (i = 0; i + 3 <= n && o + 4 < cap; i += 3, o += 4) {
        uint32_t v = (p[i] << 16) | (p[i+1] << 8) | p[i+2];
        out[o] = T[v >> 18]; out[o+1] = T[(v >> 12) & 63];
        out[o+2] = T[(v >> 6) & 63]; out[o+3] = T[v & 63];
    }
    if (i < n && o + 4 < cap) {
        uint32_t v = (p[i] << 16) | ((i+1 < n ? p[i+1] : 0) << 8);
        out[o] = T[v >> 18]; out[o+1] = T[(v >> 12) & 63];
        out[o+2] = (i+1 < n) ? T[(v >> 6) & 63] : '=';
        out[o+3] = '=';
        o += 4;
    }
    out[o] = 0;
    return (int)o;
}

void DbgPoke(int32_t module) {
    int32_t* s = Slot1();
    WriteLog("[MP-AV] poke %d (was %d)\n", module, s ? *s : -9);
    if (s && module >= 0) *s = module;
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

} // namespace mp::avatar
