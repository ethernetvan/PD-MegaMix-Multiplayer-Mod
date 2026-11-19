// PC port of switch-client-mod MpModuleDb.cpp: the merged module table is
// read straight out of memory (no parser hook needed on PC — the table is a
// fixed global). Two vectors of 0xC0-byte entries at exe+0x16EBEE8:
//   begin/end at +0x00/+0x08 and +0x18/+0x20
//   +0x00 id, +0x04 sort_index, +0x08 chara, +0x0C cos
//   +0x6C SPR_SEL_MDnnnCMN sprite-set id, +0x74 its MD_IMG sprite id
//   +0x80 prj::string name (MSVC: size +0x90, cap +0x98, inline when cap<16)
#include "../Stubs.hpp"
#include "../Config.hpp"
#include "../platform/MpLog.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace moduledb {

static uintptr_t Exe() { return (uintptr_t)GetModuleHandleW(nullptr); }

enum : size_t {
    OFF_TBL = 0x16EBEE8,
    ENT_SIZE = 0xC0,
    E_ID = 0x00, E_SORT = 0x04, E_CHARA = 0x08, E_COS = 0x0C,
    E_SET = 0x6C, E_SPR = 0x74, E_NAME = 0x80,
};

static const uint8_t* TblBase(int v) {
    const uint8_t* h = (const uint8_t*)(Exe() + OFF_TBL);
    return *(const uint8_t* const*)(h + v * 0x18);
}
static const uint8_t* TblEnd(int v) {
    const uint8_t* h = (const uint8_t*)(Exe() + OFF_TBL);
    return *(const uint8_t* const*)(h + v * 0x18 + 8);
}

static bool Entries(const uint8_t** b, size_t* n) {
    const uint8_t* beg = TblBase(0), *end = TblEnd(0);
    if (!beg || end <= beg || (end - beg) % ENT_SIZE) return false;
    size_t cnt = (size_t)(end - beg) / ENT_SIZE;
    if (cnt > 20000) return false;
    /* sanity-check the first entry so early boot (empty vectors) fails
     * cleanly instead of handing out a wrong table */
    int32_t id = *(const int32_t*)beg;
    if (id < 0 || id > 99999) return false;
    *b = beg; *n = cnt;
    return true;
}

bool Available() {
    const uint8_t* b; size_t n;
    return Entries(&b, &n);
}

void MarkReady() {
    static bool s_logged = false;
    if (s_logged) return;
    const uint8_t* b; size_t n;
    if (Entries(&b, &n)) {
        s_logged = true;
        WriteLog("[MP-MDB] table %zu entries @%p\n", n, b);
    }
}

static uint32_t Fnv(uint32_t h, const void* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)p)[i]; h *= 16777619u; }
    return h;
}

static void Fill(const uint8_t* e, Info* out) {
    out->id = *(const int32_t*)(e + E_ID);
    out->sortIndex = *(const int32_t*)(e + E_SORT);
    out->chara = *(const int32_t*)(e + E_CHARA);
    out->cos = *(const int32_t*)(e + E_COS);
    uint32_t h = Fnv(2166136261u, &out->chara, 4);
    h = Fnv(h, &out->cos, 4);
    h = Fnv(h, &out->sortIndex, 4);
    out->sig = h ? h : 1;
}

static const uint8_t* Entry(int32_t id) {
    for (int v = 0; v < 2; v++) {
        const uint8_t* b = TblBase(v), *e = TblEnd(v);
        for (const uint8_t* p = b; p && p + ENT_SIZE <= e; p += ENT_SIZE)
            if (*(const int32_t*)(p + E_ID) == id) return p;
    }
    return nullptr;
}

bool Find(int32_t module, Info* out) {
    if (!Available()) return false;
    const uint8_t* e = Entry(module);
    if (!e) return false;
    Fill(e, out);
    return true;
}

int32_t DefaultModuleFor(int32_t chara) {
    /* lowest id per chara (Hatsune Miku, Kagamine Rin, Kagamine Len,
     * Megurine Luka, Akita Neru, Yowane Haku, KAITO Original,
     * MEIKO Original, Sakine Meiko, Kasane Teto) */
    static const int32_t kDef[10] = { 0, 40, 41, 42, 2, 1, 45, 44, 43, 283 };
    if (chara < 0 || chara >= 10 || !Available()) return -1;
    if (Entry(kDef[chara])) return kDef[chara];
    /* id missing on PC: first table entry for the character */
    const uint8_t* b; size_t n;
    Entries(&b, &n);
    for (size_t i = 0; i < n; i++)
        if (*(const int32_t*)(b + i * ENT_SIZE + E_CHARA) == chara)
            return *(const int32_t*)(b + i * ENT_SIZE + E_ID);
    return -1;
}

/* MSVC std::string at entry+0x80: buf/ptr +0x00, size +0x10, cap +0x18 */
static const char* StrData(const uint8_t* s, size_t* n) {
    size_t cap = *(const size_t*)(s + 0x18);
    *n = *(const size_t*)(s + 0x10);
    const char* p = cap < 16 ? (const char*)s : *(const char* const*)s;
    if (!p || *n > 512) { *n = 0; return ""; }
    return p;
}

void DbgDump() {
    const uint8_t* b; size_t n;
    if (!Entries(&b, &n)) {
        WriteLog("[MP-MDB] dump: table not loaded\n");
        return;
    }
    std::string path = Config::PathA("module_tbl_dump.txt");
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { WriteLog("[MP-MDB] dump: fopen failed %s\n", path.c_str()); return; }
    for (size_t i = 0; i < n; i++) {
        const uint8_t* e = b + i * ENT_SIZE;
        Info inf; Fill(e, &inf);
        size_t nl; const char* nm = StrData(e + E_NAME, &nl);
        fprintf(f, "%d %d %d %d set=%d spr=%d %u %.*s\n",
                inf.id, inf.sortIndex, inf.chara, inf.cos,
                *(const int32_t*)(e + E_SET), *(const int32_t*)(e + E_SPR),
                inf.sig, (int)(nl > 256 ? 256 : nl), nm);
    }
    fclose(f);
    WriteLog("[MP-MDB] dumped %zu modules to %s\n", n, path.c_str());
}

/* avatar.cpp reads the sprite ids straight off the same entry */
const uint8_t* RawEntry(int32_t id) { return Available() ? Entry(id) : nullptr; }

} // namespace moduledb
