#include "NxShim.hpp"
#include <windows.h>
#undef CreateFile
#undef DeleteFile
#undef CreateDirectory
#undef MoveFile
#undef CopyFile
#include <mutex>
#include <atomic>
#include <cstdio>
#include <string>
#include <random>

namespace nn { namespace os {
static LARGE_INTEGER s_freq = { 0 };
uint64_t s_tickFreq() {
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    return (uint64_t)s_freq.QuadPart;
}
Tick GetSystemTick() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return { c.QuadPart };
}
}} // namespace nn::os

namespace nn { namespace fs {
static std::wstring s_modDir;
void FsSetModDir(const wchar_t* modDir) { s_modDir = modDir ? modDir : L""; }

std::string FsTranslatePath(const char* path) {
    std::string p = path ? path : "";
    /* Strip "ExlSD:/" style prefixes; keep basename under the mod dir. */
    size_t sl = p.find_last_of("/\\");
    std::string base = (sl != std::string::npos) ? p.substr(sl + 1) : p;
    std::string dir;
    for (wchar_t c : s_modDir) dir += (char)c;
    if (!dir.empty() && dir.back() != '\\') dir += '\\';
    return dir + base;
}

Result OpenFile(FileHandle* out, const char* path, int mode) {
    if (!out || !path) return 1;
    std::string p = FsTranslatePath(path);
    const wchar_t* wmode = L"rb";
    if (mode == OpenMode_Write) wmode = L"wb";
    else if (mode == OpenMode_Append) wmode = L"ab";
    std::wstring wp(p.begin(), p.end());
    FILE* f = nullptr;
    if (_wfopen_s(&f, wp.c_str(), wmode) != 0 || !f) return 1;
    out->f = f;
    return 0;
}
Result CreateFile(const char* path, int64_t) {
    std::string p = FsTranslatePath(path);
    std::wstring wp(p.begin(), p.end());
    FILE* f = nullptr;
    if (_wfopen_s(&f, wp.c_str(), L"wb") != 0 || !f) return 1;
    fclose(f);
    return 0;
}
Result GetFileSize(int64_t* out, const FileHandle& h) {
    if (!out || !h.f) return 1;
    long cur = ftell(h.f);
    fseek(h.f, 0, SEEK_END);
    *out = (int64_t)ftell(h.f);
    fseek(h.f, cur, SEEK_SET);
    return 0;
}
Result ReadFile(const FileHandle& h, int64_t off, void* buf, uint64_t len) {
    if (!h.f || (!buf && len)) return 1;
    fseek(h.f, (long)off, SEEK_SET);
    return fread(buf, 1, (size_t)len, h.f) == len ? 0 : 1;
}
Result WriteFile(const FileHandle& h, int64_t off, const void* buf, uint64_t len, WriteOption) {
    if (!h.f || (!buf && len)) return 1;
    fseek(h.f, (long)off, SEEK_SET);
    size_t w = fwrite(buf, 1, (size_t)len, h.f);
    fflush(h.f);
    return w == len ? 0 : 1;
}
Result DeleteFile(const char* path) {
    std::string p = FsTranslatePath(path);
    std::wstring wp(p.begin(), p.end());
    return _wremove(wp.c_str()) == 0 ? 0 : 1;
}
void CloseFile(FileHandle& h) {
    if (h.f) { fclose(h.f); h.f = nullptr; }
}
}} // namespace nn::fs

namespace exl { namespace util {
ModuleInfo GetMainModuleInfo() {
    ModuleInfo mi{};
    mi.m_Total.m_Start = (uintptr_t)GetModuleHandleW(nullptr);
    return mi;
}
uint64_t GetRandomU64() {
    static std::mt19937_64 rng([] {
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (uint64_t)c.QuadPart ^ (uint64_t)GetCurrentProcessId() << 32;
    }());
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    return rng();
}
}} // namespace exl::util
