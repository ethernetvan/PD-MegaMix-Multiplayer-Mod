#pragma once
// PC port: minimal shims for the Horizon SDK (nn::os, nn::hid, nn::fs) and the
// exlaunch helpers (exl::util, libcxx_vector_base, Result/R_* macros) that the
// Switch mp/ sources use. Behaviour matches the Switch semantics the mp code
// relies on; implementations live in NxShim.cpp.
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <thread>
#include <string>

/* nn::Result as a plain int; R_SUCCEEDED/R_FAILED mirror Switch code. */
using Result = int;
#ifndef R_SUCCEEDED
#define R_SUCCEEDED(r) ((r) == 0)
#endif
#ifndef R_FAILED
#define R_FAILED(r) ((r) != 0)
#endif

/* Win32 defines CreateFile/DeleteFile as *W/*A macros; the Switch shim
 * needs the plain names inside nn::fs. */
#undef CreateFile
#undef DeleteFile
#undef CreateDirectory
#undef MoveFile
#undef CopyFile

namespace nn {

using TimeTickSystem = uint64_t;

struct TimeSpan {
    int64_t ns;
    static TimeSpan FromMilliSeconds(uint64_t ms) { return { (int64_t)ms * 1000000 }; }
    static TimeSpan FromSeconds(uint64_t s) { return { (int64_t)s * 1000000000 }; }
};

namespace os {
struct Tick {
    int64_t v;
    int64_t GetInt64Value() const { return v; }
};
uint64_t s_tickFreq();
Tick GetSystemTick();
inline uint64_t GetSystemTickFrequency() { return s_tickFreq(); }
inline void SleepThread(const TimeSpan& ts) {
    if (ts.ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(ts.ns));
}
inline void YieldThread() { std::this_thread::yield(); }
} // namespace os

namespace hid {
/* Logical button mask bits. Same bit layout as nn::hid::NpadButton on Switch
 * so the mp code's masks (Minus = bit 9 etc.) carry over unchanged. */
namespace Button {
constexpr uint64_t A      = 1ull << 0;
constexpr uint64_t B      = 1ull << 1;
constexpr uint64_t X      = 1ull << 2;
constexpr uint64_t Y      = 1ull << 3;
constexpr uint64_t L      = 1ull << 6;
constexpr uint64_t R      = 1ull << 7;
constexpr uint64_t ZL     = 1ull << 8;
constexpr uint64_t ZR     = 1ull << 9;
constexpr uint64_t Plus   = 1ull << 10;
constexpr uint64_t Minus  = 1ull << 11;
constexpr uint64_t Left   = 1ull << 12;
constexpr uint64_t Up     = 1ull << 13;
constexpr uint64_t Right  = 1ull << 14;
constexpr uint64_t Down   = 1ull << 15;
constexpr uint64_t LStick = 1ull << 16;
constexpr uint64_t RStick = 1ull << 17;
} // namespace Button
using namespace Button; /* nn::hid::A etc. */

struct NpadHandheldState {
    uint64_t buttons;
    int32_t analogStickL[2];
    int32_t analogStickR[2];
    NpadHandheldState() : buttons(0) { analogStickL[0]=analogStickL[1]=0; analogStickR[0]=analogStickR[1]=0; }
};

/* Implemented in MpInput.cpp: merged logical state (all input states +
 * injections + synthesised Minus). */
NpadHandheldState GetMergedNpadState();
} // namespace hid

namespace fs {
enum OpenMode : int {
    OpenMode_Read  = 1,
    OpenMode_Write = 2,
    OpenMode_Append = 3,
};
enum WriteOptionFlag : int { WriteOptionFlag_Flush = 1 };
struct WriteOption {
    int v;
    static WriteOption CreateOption(int flag) { return { flag }; }
};
struct FileHandle { FILE* f; };
Result OpenFile(FileHandle* out, const char* path, int mode);
Result CreateFile(const char* path, int64_t size);
Result GetFileSize(int64_t* out, const FileHandle& h);
Result ReadFile(const FileHandle& h, int64_t off, void* buf, uint64_t len);
Result WriteFile(const FileHandle& h, int64_t off, const void* buf, uint64_t len, WriteOption opt);
Result DeleteFile(const char* path);
void   CloseFile(FileHandle& h);
/* "ExlSD:/..." -> file under the mod directory. */
std::string FsTranslatePath(const char* path);
void FsSetModDir(const wchar_t* modDir);
} // namespace fs
} // namespace nn

namespace exl { namespace util {
struct ModuleInfo { struct { uintptr_t m_Start; } m_Total; };
ModuleInfo GetMainModuleInfo();
uint64_t GetRandomU64();
}} // namespace exl::util

/* libc++ vector layout (matches both the Switch libnx STL and MSVC's
 * _Vector_val { _Myfirst, _Mylast, _Myend } triple-pointer layout). */
template <typename T>
struct libcxx_vector_base {
    T* begin_;
    T* end_;
    T* cap_;
};
