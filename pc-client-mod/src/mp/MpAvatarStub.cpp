// PC port: avatar stub for H1. No module-table read and no GPU readback yet,
// so every portrait resolves to "no image" and the UI draws its silhouette
// fallback. Announce {"t":"avatar","module":-1,"chara":-1,"mh":0}.
#include "MpAvatar.hpp"
#include "MpNet.hpp"
#include "../platform/MpLog.hpp"
#include <windows.h>
#include <cstring>
#include <cstdio>

namespace mp::avatar {

void CaptureSaveMgr(void*) {}
int32_t LocalModule() { return -1; }

void OnWelcome() {
    mp::Enqueue("{\"t\":\"avatar\",\"module\":-1,\"chara\":-1,\"mh\":0}");
}

/* ---- RE probe (dbg_cmd avatar_dump N): find a module card's texture ----
 * Module table: handler at exe+0x16EBEE8, two vectors of 0xC0-byte
 * entries (+0 id, +0x6C CMN spr set id, +0x74 CMN md img spr id). */
static uintptr_t Exe() { static uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr); return b; }
static const uint8_t* ModuleEntry(int32_t id) {
    const uint8_t* h = (const uint8_t*)(Exe() + 0x16EBEE8);
    for (int v = 0; v < 2; v++) {
        const uint8_t* b = *(const uint8_t* const*)(h + v * 0x18);
        const uint8_t* e = *(const uint8_t* const*)(h + v * 0x18 + 8);
        for (const uint8_t* p = b; p && p < e; p += 0xC0)
            if (*(const int32_t*)p == id) return p;
    }
    return nullptr;
}
static int32_t s_probe = -1, s_probeStage = 0, s_probeTries = 0;
static uint8_t s_strv[32];

void GamePump() {
    if (s_probe < 0) return;
    using LoadSet = void (__fastcall*)(uint32_t, void*);
    using Loading = bool (__fastcall*)(uint32_t);
    using DefArgs = void* (__fastcall*)(void*);
    using PutSpr  = uint8_t* (__fastcall*)(void*);
    const uint8_t* e = ModuleEntry(s_probe);
    if (!e) { WriteLog("[MP-AV] probe: module %d not in table\n", s_probe); s_probe = -1; return; }
    uint32_t set = *(const uint32_t*)(e + 0x6C), spr = *(const uint32_t*)(e + 0x74);
    if (s_probeStage == 0) {
        memset(s_strv, 0, sizeof(s_strv));
        ((LoadSet)(Exe() + 0x5B4770))(set, s_strv);
        WriteLog("[MP-AV] probe: module %d set %u spr %u - load queued\n", s_probe, set, spr);
        s_probeStage = 1; s_probeTries = 0;
        return;
    }
    if (((Loading)(Exe() + 0x5B4810))(set)) {
        if (++s_probeTries % 60 == 0) WriteLog("[MP-AV] probe: loading...\n");
        return;
    }
    alignas(16) static uint8_t args[0x200];
    memset(args, 0, sizeof(args));
    ((DefArgs)(Exe() + 0x5B78D0))(args);
    *(uint32_t*)(args + 4) = spr;
    args[8] = args[9] = args[10] = 255; args[11] = 0;   /* invisible */
    uint8_t* q = ((PutSpr)(Exe() + 0x5B49C0))(args);
    void* tex = q ? *(void**)(q + 0xA0) : nullptr;
    WriteLog("[MP-AV] probe: put_sprite -> %p tex=%p (args tex=%p)\n", (void*)q, tex,
             *(void**)(args + 0xA0));
    if (q) {
        const uint8_t* b = q;
        for (int r = 0; r < 0x140; r += 32) {
            char line[200]; int o = 0;
            for (int i = 0; i < 32; i++) o += snprintf(line + o, sizeof(line) - o, "%02x", b[r + i]);
            WriteLog("[MP-AV]   +%03x %s\n", r, line);
        }
    }
    if (tex) {
        const uint8_t* t = (const uint8_t*)tex;
        WriteLog("[MP-AV] dx_texture: d3d=%p srv=%p fmt=%u ifmt=%u flags=%x %ux%u mips=%u\n",
                 *(void* const*)(t + 0x10), *(void* const*)(t + 0x18), *(const uint32_t*)(t + 0x20),
                 *(const uint32_t*)(t + 0x24), *(const uint32_t*)(t + 0x28),
                 *(const uint32_t*)(t + 0x2C), *(const uint32_t*)(t + 0x30), *(const uint32_t*)(t + 0x34));
    }
    s_probe = -1; s_probeStage = 0;
}
void RenderPump() {}

int Get(int32_t, void**, float[2], float[2]) { return -1; }
void Prefetch(int32_t) {}
int Resolve(const Ref&, void**, float[2], float[2]) { return -1; }
void OnRemotePixels(const char*, const char*, int, bool) {}
void DropRemote() {}
void DbgPoke(int32_t) {}
void DbgPeek() {}
void DbgDump(int32_t module) { s_probe = module; s_probeStage = 0; }

} // namespace mp::avatar
