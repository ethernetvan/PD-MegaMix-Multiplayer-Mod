#include "MpModuleDb.hpp"
#include "../lib.hpp"
#include "../logger.hpp"
#include <nn.hpp>
#include <nn/fs.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mp::moduledb {

/* FUN_003c3980(db, kv): parses one module table file (module.N.id/chara/
 * cos/name/...) and merges it into a std::vector at db+0x18 (begin) /
 * db+0x20 (end) of 0x40-byte entries sorted by the int at +0:
 *   +0x00 key id, +0x08 id, +0x0C sort_index, +0x10 std::string name,
 *   +0x28 chara, +0x2C cos, +0x30 attr, +0x34 shop_price, +0x38 u8 ng */
static std::atomic<uint8_t*> s_db{nullptr};
static std::atomic<bool>     s_ready{false};

HOOK_DEFINE_TRAMPOLINE(MpModuleTblParseHook) {
    static void Callback(void* db, void* kv) {
        Orig(db, kv);
        s_db.store((uint8_t*)db);
    }
};

void Init() { MpModuleTblParseHook::InstallAtOffset(0x3c3980); }

void MarkReady() {
    if (!s_ready.exchange(true)) {
        uint8_t* db = s_db.load();
        WriteLog("[MP-MDB] ready db=%p\n", db);
    }
}

static bool Entries(const uint8_t** b, size_t* n) {
    uint8_t* db = s_db.load();
    if (!db || !s_ready.load()) return false;
    const uint8_t* beg = *(const uint8_t* const*)(db + 0x18);
    const uint8_t* end = *(const uint8_t* const*)(db + 0x20);
    if (!beg || end <= beg || (end - beg) % 0x40) return false;
    size_t cnt = (size_t)(end - beg) / 0x40;
    if (cnt > 20000) return false;
    *b = beg; *n = cnt;
    return true;
}

bool Available() {
    const uint8_t* b; size_t n;
    return Entries(&b, &n);
}

/* libc++ std::string: short if byte0 bit0 clear (size = byte0 >> 1) */
static const char* StrData(const uint8_t* s, size_t* n) {
    if ((s[0] & 1) == 0) { *n = s[0] >> 1; return (const char*)s + 1; }
    *n = *(const size_t*)(s + 8);
    const char* p = *(const char* const*)(s + 0x10);
    if (!p || *n > 512) { *n = 0; return ""; }
    return p;
}

static uint32_t Fnv(uint32_t h, const void* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)p)[i]; h *= 16777619u; }
    return h;
}

static void Fill(const uint8_t* e, Info* out) {
    out->id = *(const int32_t*)(e + 0x08);
    out->sortIndex = *(const int32_t*)(e + 0x0C);
    out->chara = *(const int32_t*)(e + 0x28);
    out->cos = *(const int32_t*)(e + 0x2C);
    uint32_t h = Fnv(2166136261u, &out->chara, 4);
    h = Fnv(h, &out->cos, 4);
    h = Fnv(h, &out->sortIndex, 4);
    out->sig = h ? h : 1;
}

int32_t DefaultModuleFor(int32_t chara) {
    /* lowest id per chara in the 1.0.6 table (Hatsune Miku, Kagamine Rin,
     * Kagamine Len, Megurine Luka, Akita Neru, Yowane Haku, KAITO Original,
     * MEIKO Original, Sakine Meiko, Kasane Teto) */
    static const int32_t kDef[10] = { 0, 40, 41, 42, 2, 1, 45, 44, 43, 283 };
    return chara >= 0 && chara < 10 ? kDef[chara] : -1;
}

bool Find(int32_t module, Info* out) {
    const uint8_t* b; size_t n;
    if (!Entries(&b, &n)) return false;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int32_t k = *(const int32_t*)(b + mid * 0x40);
        if (k == module) { Fill(b + mid * 0x40, out); return true; }
        if (k < module) lo = mid + 1; else hi = mid;
    }
    return false;
}

void DbgDump() {
    const uint8_t* b; size_t n;
    if (!Entries(&b, &n)) {
        WriteLog("[MP-MDB] dump: table not ready (db=%p ready=%d)\n", s_db.load(), (int)s_ready.load());
        return;
    }
    size_t cap = 4096 + n * 160;
    char* buf = (char*)malloc(cap);
    if (!buf) return;
    size_t o = (size_t)snprintf(buf, cap, "db=%p n=%zu\n", s_db.load(), n);
    for (size_t k = 0; k < 2 && k < n; k++) {
        for (int i = 0; i < 0x40 && o + 4 < cap; i++)
            o += (size_t)snprintf(buf + o, cap - o, "%02x%s", b[k * 0x40 + i], i == 0x3f ? "\n" : " ");
    }
    for (size_t k = 0; k < n && o + 600 < cap; k++) {
        const uint8_t* e = b + k * 0x40;
        Info inf; Fill(e, &inf);
        size_t nl; const char* nm = StrData(e + 0x10, &nl);
        o += (size_t)snprintf(buf + o, cap - o, "%d %d %d %d %d %u %08x %.*s\n",
                              inf.id, inf.sortIndex, inf.chara, inf.cos,
                              *(const int32_t*)(e + 0x30), (unsigned)e[0x38],
                              inf.sig, (int)(nl > 256 ? 256 : nl), nm);
    }
    const char* path = "ExlSD:/module_tbl_dump.txt";
    nn::fs::DeleteFile(path);
    nn::fs::FileHandle f;
    if (R_SUCCEEDED(nn::fs::CreateFile(path, (int64_t)o)) &&
        R_SUCCEEDED(nn::fs::OpenFile(&f, path, nn::fs::OpenMode_Write))) {
        nn::fs::WriteFile(f, 0, buf, o, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
        nn::fs::CloseFile(f);
        WriteLog("[MP-MDB] dumped %zu modules to %s\n", n, path);
    }
    free(buf);
}

}
