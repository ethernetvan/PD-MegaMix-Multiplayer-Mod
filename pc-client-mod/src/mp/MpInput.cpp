// PC port of MpInput.cpp — same tap/spam/block/nav semantics as the Switch
// version; the backend edits the game's InputState key bitsets instead of an
// nn::hid::NpadHandheldState (hook on PollInputRepeatAndDouble).
//
// InputState (MM+ 1.03, verified via xdis):
//   0xA2 keys, 64-bit words: tapped @0x00, down @0x30, repeat-tap @0x90,
//   abs-tapped @0x19C8. Stick positions: int32 @0xE0 + index*4 (0x14..0x17).
// Logical nn::hid bits <-> key indices live in game/keys.h.
//
// Emergency exit on PC: hold F9 for 3 s or hold a controller's Back button
// for 3 s while the game window is foreground -> ResetAll (the Minus bit is
// synthesised for the merged-state readers; Minus is never blocked anyway).
#include "MpInput.hpp"
#include "MpMenu.hpp"
#include "MpLobby.hpp"
#include "MpNet.hpp"
#include "../platform/MpLog.hpp"
#include "../platform/NxShim.hpp"
#include "../game/diva.h"
#include "../game/keys.h"
#include <windows.h>
#include <tlhelp32.h>
#include <xinput.h>
#ifdef MP_DEV_TOOLS
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif
#include <safetyhook/safetyhook.hpp>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <map>
#include <atomic>
#include <intrin.h>

#pragma comment(lib, "xinput.lib")
#ifdef MP_DEV_TOOLS
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#endif

namespace mp::input {
static bool GameForeground();

struct TapEntry {
    uint64_t buttons;
    uint32_t hold_ms;
    uint32_t gap_ms;
    bool     force;
};

static TapEntry    s_taps[32];
static uint32_t    s_tapHead = 0, s_tapCount = 0;
static uint64_t    s_tapStartTick = 0;
static std::mutex  s_mtx;
static uint64_t    s_blocked = 0;
static bool        s_zeroSticks = false;
static int         s_bypassDepth = 0;
static DWORD       s_bypassTid = 0;
/* per-source real (pre-mask) buttons + last-seen ms: Apply runs once per
 * InputState poll, so edge latching ORs sources seen within 250 ms — a held
 * button must not look like a fresh edge on every poll. PC: one slot per
 * distinct InputState* (keyboard+pad are merged by the game per state). */
static uint64_t    s_srcReal[27] = {};
static uint64_t    s_srcRealMs[27] = {};
static uint64_t    s_prevRealComb = 0;
static uint64_t    s_realEdges = 0;     /* latched rising edges, real input */
static uint64_t    s_navHeld = 0;
static uint64_t    s_navPrev = 0;
static uint64_t    s_navEdges = 0;      /* latched nav rising edges */
static int32_t     s_navLx = 0, s_navLy = 0;

static DWORD CurTid() { return GetCurrentThreadId(); }

static struct {
    bool     active = false;
    uint64_t buttons = 0;
    uint32_t hz = 0;
    uint64_t startTick = 0;
    uint64_t endTick = 0;
} s_spam;

static uint64_t NowTick() {
    return nn::os::GetSystemTick().GetInt64Value();
}
static uint64_t NowMs() {
    return NowTick() * 1000 / nn::os::GetSystemTickFrequency();
}
static uint64_t MsToTick(uint64_t ms) {
    return ms * nn::os::GetSystemTickFrequency() / 1000;
}

void Tap(uint64_t buttons, uint32_t hold_ms, uint32_t gap_ms, bool force) {
    if (buttons == 0) return;
    if (buttons & nn::hid::Plus) {
        /* pause is not an input on PC: flip the native pause target (see
         * "native pause control" below) for a short one-shot window */
        PauseWant(GamePaused() ? 0 : 1, 1500);
        buttons &= ~nn::hid::Plus;
        if (!buttons) return;
    }
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_tapCount == 32) {
        s_tapHead = (s_tapHead + 1) & 31;
        s_tapCount--;
    }
    uint32_t idx = (s_tapHead + s_tapCount) & 31;
    s_taps[idx] = {buttons, hold_ms, gap_ms, force};
    if (s_tapCount == 0) s_tapStartTick = NowTick();
    s_tapCount++;
}

void Spam(uint64_t buttons, uint32_t hz, uint32_t secs) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (hz == 0 || secs == 0 || buttons == 0) {
        s_spam.active = false;
        return;
    }
    s_spam.active = true;
    s_spam.buttons = buttons;
    s_spam.hz = hz;
    s_spam.startTick = NowTick();
    s_spam.endTick = s_spam.startTick + MsToTick((uint64_t)secs * 1000);
}

void StopSpam() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_spam.active = false;
}

void SetBlocked(uint64_t mask) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_blocked = mask & ~nn::hid::Minus;   /* Minus = emergency exit, never blocked */
}
void ClearBlocked() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_blocked = 0;
}

void SetZeroSticks(bool on) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_zeroSticks = on;
}

void BeginBypass() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_bypassDepth == 0) s_bypassTid = CurTid();
    s_bypassDepth++;
}
void EndBypass() {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_bypassDepth > 0) s_bypassDepth--;
}

uint64_t ConsumeRealPress(uint64_t mask) {
    std::lock_guard<std::mutex> lk(s_mtx);
    uint64_t r = s_realEdges & mask;
    s_realEdges &= ~mask;
    return r;
}

/* ---------------- nav state (merged path) ---------------- */

void NavUpdate(const nn::hid::NpadHandheldState& st) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_navEdges |= st.buttons & ~s_navPrev;
    s_navPrev = st.buttons;
    s_navHeld = st.buttons;
    s_navLx = st.analogStickL[0];
    s_navLy = st.analogStickL[1];
}

uint64_t NavConsume(uint64_t mask) {
    std::lock_guard<std::mutex> lk(s_mtx);
    uint64_t r = s_navEdges & mask;
    s_navEdges &= ~mask;
    return r;
}

uint64_t NavHeld() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_navHeld;
}

void NavStick(int* x, int* y) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (x) *x = s_navLx;
    if (y) *y = s_navLy;
}

void NavClear() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_navEdges = 0;
    s_navPrev = s_navHeld;
}

void ResetAll() {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_blocked = 0;
    s_zeroSticks = false;
    s_tapCount = 0;
    s_spam.active = false;
    mp::lobby::ClearGroupPause();

}

/* ---------------- raw-index tap injection (dbg_cmd rawtap) ---------------- */
struct RawTapEntry { int32_t idx; uint64_t endTick; };
#ifdef MP_DEV_TOOLS
static RawTapEntry s_rawTaps[8];
static int s_rawTapN = 0;

void RawTap(int32_t idx, uint32_t hold_ms) {
    if (idx < 0 || idx >= (int32_t)addr::InputKeyCount) return;
    std::lock_guard<std::mutex> lk(s_mtx);
    for (int i = 0; i < s_rawTapN; i++) {
        if (s_rawTaps[i].idx == idx) {
            s_rawTaps[i].endTick = NowTick() + MsToTick(hold_ms);
            return;
        }
    }
    if (s_rawTapN < 8) {
        s_rawTaps[s_rawTapN].idx = idx;
        s_rawTaps[s_rawTapN].endTick = NowTick() + MsToTick(hold_ms);
        s_rawTapN++;
    }
}
#else
void RawTap(int32_t, uint32_t) {}
#endif

#ifdef MP_DEV_TOOLS
static std::atomic<int> s_keylog{0};
static std::mutex   s_kmu;
static char          s_kbuf[16384];
static size_t        s_klen = 0;
static ULONGLONG     s_kflushAt = 0;
/* buffered, append-only keylog: one WriteLog per Apply pass at most -
 * no mutex/file I/O per line (per-line logging deadlocked the game) */
void KLog(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    std::lock_guard<std::mutex> lk(s_kmu);
    if (s_klen + (size_t)n + 1 > sizeof(s_kbuf)) return;   /* drop on full */
    memcpy(s_kbuf + s_klen, line, (size_t)n);
    s_klen += (size_t)n;
    s_kbuf[s_klen] = 0;
}
static void KLogFlush() {
    {   std::lock_guard<std::mutex> lk(s_kmu);
        if (!s_klen) return;
        if (GetTickCount64() < s_kflushAt && s_klen < sizeof(s_kbuf) - 1024)
            return;                        /* ~30 Hz max */
        s_kflushAt = GetTickCount64() + 33;
    }
    char tmp[16384];
    {   std::lock_guard<std::mutex> lk(s_kmu);
        memcpy(tmp, s_kbuf, s_klen + 1);
        s_klen = 0; s_kbuf[0] = 0;
    }
    WriteLog("%s", tmp);
}
#else
void KLog(const char*, ...) {}
static void KLogFlush() {}
#endif

#ifdef MP_DEV_TOOLS
/* ---------------- hardware write-watchpoint (RE aid) ----------------
 * Arms DR0 as a 1-byte write breakpoint on the given address on every
 * current thread; the VEH logs the RIP after the write plus plausible
 * return addresses off the stack. Buffered through KLog — no file I/O
 * on the faulting thread beyond a short mutex. */
static PVOID     s_watchVeh = nullptr;
static uintptr_t s_watchAddr = 0;
static int       s_watchHits = 0;

static LONG WINAPI WatchVeh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP ||
        !(ep->ContextRecord->Dr6 & 0xF))
        return EXCEPTION_CONTINUE_SEARCH;
    /* Self-disarm FIRST: zeroing Dr7 in the exception context is written back
     * on continue, so this thread's breakpoint fires at most once. Without
     * this the trap fires on every subsequent write and storm-locks the game
     * (observed: hung window at watchp + first vktap). */
    ep->ContextRecord->Dr7 = 0;
    ep->ContextRecord->Dr6 = 0;
    if (!s_watchAddr || s_watchHits >= 16)
        return EXCEPTION_CONTINUE_EXECUTION;
    s_watchHits++;
    uintptr_t rip = ep->ContextRecord->Rip;
    uintptr_t rsp = ep->ContextRecord->Rsp;
    uintptr_t rets[10]; int n = 0;
    __try {
        for (int i = 0; i < 0x40 && n < 10; i++) {
            uintptr_t v = *(uintptr_t*)(rsp + (size_t)i * 8);
            if (v >= 0x140001000 && v < 0x140b25000) rets[n++] = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    char line[320]; int o = snprintf(line, sizeof line,
        "[MP] watch rip=%llx tid=%lu", (unsigned long long)rip,
        (unsigned long)GetCurrentThreadId());
    for (int i = 0; i < n; i++)
        o += snprintf(line + o, sizeof line - o, " %llx",
                      (unsigned long long)rets[i]);
    o += snprintf(line + o, sizeof line - o, "\n");
    KLog("%s", line);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void SetDrAll(uintptr_t a, bool on) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        if (te.th32ThreadID == self) continue;   /* suspending ourselves deadlocks OnFrame */
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                if (on) { c.Dr0 = a; c.Dr7 |= 1ull | (1ull << 16); }
                else    { c.Dr0 = 0; c.Dr7 &= ~(1ull | (0xFull << 16)); }
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}

/* A thread can't suspend/arm itself — a helper suspends the caller's thread
 * for a moment and sets its DR0 (safe: it runs after WatchAddr returns). */
struct ArmSelfArgs { DWORD tid; uintptr_t addr; int on; };
static DWORD WINAPI ArmSelfThread(void* p) {
    ArmSelfArgs* a = (ArmSelfArgs*)p;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                          THREAD_SET_CONTEXT, FALSE, a->tid);
    if (t) {
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                if (a->on) { c.Dr0 = a->addr; c.Dr7 |= 1ull | (1ull << 16); }
                else       { c.Dr0 = 0; c.Dr7 &= ~(1ull | (0xFull << 16)); }
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    delete a;
    return 0;
}

void WatchAddr(uintptr_t a) {
    s_watchHits = 0;
    s_watchAddr = a;
    if (!s_watchVeh) s_watchVeh = AddVectoredExceptionHandler(1, WatchVeh);
    WriteLog("[MP] watch arming %llx\n", (unsigned long long)a);
    SetDrAll(a, true);
    ArmSelfArgs* args = new ArmSelfArgs{ GetCurrentThreadId(), a, 1 };
    CloseHandle(CreateThread(nullptr, 0, ArmSelfThread, args, 0, nullptr));
    WriteLog("[MP] watch armed\n");
}
void WatchOff() {
    SetDrAll(0, false);
    ArmSelfArgs* args = new ArmSelfArgs{ GetCurrentThreadId(), 0, 0 };
    CloseHandle(CreateThread(nullptr, 0, ArmSelfThread, args, 0, nullptr));
    s_watchAddr = 0;
    WriteLog("[MP] watch off (hits=%d)\n", s_watchHits);
}
/* ---------------- DirectInput8 raw-path injection ----------------
 * PC scene input (menus, attract reel, results screens, pause menu) does
 * NOT read the InputState bitsets — it polls the keyboard through
 * IDirectInputDevice8::GetDeviceState/GetDeviceData. The bitset writes
 * above only feed consumers that use InputState (e.g. the PV selector).
 * Hooking the DI8 device methods (vtable addresses taken from a throwaway
 * device; COM methods are shared per class) makes injected logical buttons
 * produce real DIK presses and blocked buttons strip real keys. */
static SafetyHookInline s_gdsHook;
static SafetyHookInline s_gddHook;
static uint64_t s_diForce = 0;   /* logical bits to hold at DIK level */
static uint64_t s_diBlock = 0;   /* logical bits to remove            */
static bool     s_diBlockAll = false;
static int      s_gdsCalls = 0, s_gddCalls = 0;

static int LogicalToDik(uint64_t bit) {
    switch (bit) {
    case 1ull << 0:  return DIK_RETURN;
    case 1ull << 1:  return DIK_ESCAPE;
    case 1ull << 3:  return DIK_F7;
    case 1ull << 10: return DIK_ESCAPE;   /* Plus = Esc on PC */
    case 1ull << 13: return DIK_UP;
    case 1ull << 15: return DIK_DOWN;
    case 1ull << 12: return DIK_LEFT;
    case 1ull << 14: return DIK_RIGHT;
    default: return -1;
    }
}
static uint64_t DikToLogical(int dik) {
    switch (dik) {
    case DIK_RETURN: return 1ull << 0;
    case DIK_ESCAPE: return (1ull << 1) | (1ull << 10);
    case DIK_F7:     return 1ull << 3;
    case DIK_UP:     return 1ull << 13;
    case DIK_DOWN:   return 1ull << 15;
    case DIK_LEFT:   return 1ull << 12;
    case DIK_RIGHT:  return 1ull << 14;
    default: return 0;
    }
}

static HRESULT WINAPI DiGetDeviceStateImpl(IDirectInputDevice8W* This,
                                           DWORD cbData, LPVOID lpvData) {
    HRESULT hr = s_gdsHook.call<HRESULT>(This, cbData, lpvData);
    if (s_gdsCalls++ == 0)
        WriteLog("[MP] di8 GetDeviceState first call cb=%d\n", (int)cbData);
    if (FAILED(hr) || cbData != 256 || !lpvData) return hr;
    uint8_t* k = (uint8_t*)lpvData;
    if (s_keylog.load()) {
        static uint8_t s_prevDi[256] = {};
        for (int dik = 0; dik < 256; dik++) {
            bool now = (k[dik] & 0x80) != 0;
            bool was = (s_prevDi[dik] & 0x80) != 0;
            if (now != was)
                KLog("[MP] keylog t=%llu di_state dik=%d %d\n",
                         (unsigned long long)GetTickCount64(), dik, now);
            s_prevDi[dik] = k[dik];
        }
    }
    uint64_t force, block; bool all;
    { std::lock_guard<std::mutex> lk(s_mtx);
      force = s_diForce; block = s_diBlock; all = s_diBlockAll; }
    if (all) memset(k, 0, 256);
    else for (int dik = 0; dik < 256; dik++) {
        uint64_t lg = DikToLogical(dik);
        if (lg && (block & lg)) k[dik] &= 0x7F;
    }
    for (int i = 0; i < 64; i++) {
        uint64_t b = 1ull << i;
        if (!(force & b)) continue;
        int dik = LogicalToDik(b);
        if (dik >= 0) k[dik] |= 0x80;
    }
    return hr;
}

static HRESULT WINAPI DiGetDeviceDataImpl(IDirectInputDevice8W* This,
                                          DWORD cbObj, LPDIDEVICEOBJECTDATA rgdod,
                                          LPDWORD pdwInOut, DWORD flags) {
    HRESULT hr = s_gddHook.call<HRESULT>(This, cbObj, rgdod, pdwInOut, flags);
    if (s_gddCalls++ == 0)
        WriteLog("[MP] di8 GetDeviceData first call\n");
    if (FAILED(hr) || !rgdod || !pdwInOut) return hr;
    uint64_t force, block; bool all;
    { std::lock_guard<std::mutex> lk(s_mtx);
      force = s_diForce; block = s_diBlock; all = s_diBlockAll; }
    DWORD n = *pdwInOut;
    if (s_keylog.load())
        for (DWORD i = 0; i < n; i++)
            KLog("[MP] keylog t=%llu di_data dik=%d data=%d\n",
                     (unsigned long long)GetTickCount64(),
                     (int)(rgdod[i].dwOfs & 0xFF), (int)rgdod[i].dwData);
    for (DWORD i = 0; i < n; i++) {
        int dik = (int)(rgdod[i].dwOfs & 0xFF);
        uint64_t lg = DikToLogical(dik);
        if ((all && lg) || (lg && (block & lg))) rgdod[i].dwData &= 0x7F;
    }
    return hr;
}

void InstallDiHooks() {
    HMODULE di = GetModuleHandleW(L"dinput8.dll");
    if (!di) { WriteLog("[MP] dinput8 not loaded\n"); return; }
    auto create = (decltype(&DirectInput8Create))GetProcAddress(di, "DirectInput8Create");
    if (!create) { WriteLog("[MP] DirectInput8Create missing\n"); return; }
    IDirectInput8W* di8 = nullptr;
    if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                      IID_IDirectInput8W, (LPVOID*)&di8, nullptr)) || !di8) {
        WriteLog("[MP] DirectInput8Create failed\n"); return;
    }
    IDirectInputDevice8W* kb = nullptr;
    if (SUCCEEDED(di8->CreateDevice(GUID_SysKeyboard, &kb, nullptr)) && kb) {
        void** vt = *reinterpret_cast<void***>(kb);
        s_gdsHook = safetyhook::create_inline(vt[9], (void*)DiGetDeviceStateImpl);
        s_gddHook = safetyhook::create_inline(vt[10], (void*)DiGetDeviceDataImpl);
        WriteLog("[MP] di8 hooks: GetDeviceState @%p GetDeviceData @%p\n",
                 vt[9], vt[10]);
        kb->Release();
    } else {
        WriteLog("[MP] di8 CreateDevice failed\n");
    }
    di8->Release();
}
#else
void InstallDiHooks() {}
#endif

/* ---------------- keylog debug (dbg_cmd keylog on|off) ---------------- */
#ifdef MP_DEV_TOOLS
void SetKeylog(int on) { s_keylog = on; }
int  KeylogOn() { return s_keylog; }
#else
void SetKeylog(int) {}
int  KeylogOn() { return 0; }
#endif

#ifdef MP_DEV_TOOLS
/* ---------------- GetAsyncKeyState injection hook ----------------
 * The keyboard device update (0x1402aee40) calls the map-walk at
 * 0x1402b0cf0, which reads keys via GetAsyncKeyState through a COPIED
 * import pointer at exe+0xB257F8 (the .data2 init routine duplicates the
 * IAT entries into a local table - there are no call sites into the PE
 * IAT itself). Patching that slot makes a spoofed VK flow through the
 * real VK->index map -> device bitset (+0x518) -> InputState merge, so an
 * injected key produces the exact signature of a physical press including
 * the context-dependent aliases (gameplay Esc -> {28,160}, menu Esc ->
 * {9,28,161}). No WM messages, no DirectInput, no SendInput. */
static std::atomic<uint64_t> s_vkForce[4];
static SHORT (WINAPI* s_gaksReal)(int) = nullptr;
static int  s_gaksCalls = 0;

static SHORT WINAPI GaksDetour(int vk) {
    SHORT r = s_gaksReal ? s_gaksReal(vk) : 0;
    if (s_gaksCalls++ == 0)
        WriteLog("[MP] gaks first call vk=%d r=%x\n", vk, r & 0xffff);
    if (vk < 0 || vk > 255) return r;
    if (s_keylog.load()) {
        static uint8_t s_prevGaks[256] = {};
        bool now = (r & 0x8000) != 0;
        if (now != (s_prevGaks[vk] != 0))
            KLog("[MP] keylog t=%llu gaks vk=%d %d\n",
                 (unsigned long long)GetTickCount64(), vk, now ? 1 : 0);
        s_prevGaks[vk] = (uint8_t)now;
    }
    if (s_vkForce[vk >> 6].load(std::memory_order_relaxed) &
        (1ull << (vk & 63)))
        return (SHORT)0x8001;              /* pressed + edge */
    return r;
}

struct VkTapEntry { int vk; uint64_t endTick; };
static VkTapEntry s_vkTaps[8];
static int s_vkTapN = 0;

void VkTap(int vk, uint32_t hold_ms) {
    if (vk < 0 || vk > 255) return;
    std::lock_guard<std::mutex> lk(s_mtx);
    for (int i = 0; i < s_vkTapN; i++) {
        if (s_vkTaps[i].vk == vk) {
            s_vkTaps[i].endTick = NowTick() + MsToTick(hold_ms);
            return;
        }
    }
    if (s_vkTapN < 8) {
        s_vkTaps[s_vkTapN].vk = vk;
        s_vkTaps[s_vkTapN].endTick = NowTick() + MsToTick(hold_ms);
        s_vkTapN++;
    }
}

/* logical bit -> the VK the corresponding physical key uses (verified by
 * the real-keypress capture 2026-11-17; -1 = no VK injection) */
static int LogicalToVk(uint64_t bit) {
    switch (bit) {
    case 1ull << 0:  return VK_RETURN;   /* A     = Enter  */
    case 1ull << 1:  return VK_ESCAPE;   /* B     = Esc    */
    case 1ull << 2:  return VK_SPACE;    /* X     = Space? (preview) */
    case 1ull << 3:  return VK_F7;       /* Y     = F7     */
    case 1ull << 10: return VK_ESCAPE;   /* Plus  = Esc (pause) */
    case 1ull << 13: return VK_UP;
    case 1ull << 15: return VK_DOWN;
    case 1ull << 12: return VK_LEFT;
    case 1ull << 14: return VK_RIGHT;
    default: return -1;
    }
}

/* Rebuild the VK force mask from the active logical injects + vkTaps.
 * Called under s_mtx from Apply(). */
static void UpdateVkForce(uint64_t inject, uint64_t injectForce,
                          uint64_t now) {
    uint64_t m[4] = {};
    uint64_t bits = inject | injectForce;
    for (int i = 0; i < 64; i++) {
        if (!(bits & (1ull << i))) continue;
        int vk = LogicalToVk(1ull << i);
        if (vk >= 0) m[vk >> 6] |= 1ull << (vk & 63);
    }
    for (int i = s_vkTapN - 1; i >= 0; i--) {
        if (now >= s_vkTaps[i].endTick) {
            s_vkTaps[i] = s_vkTaps[s_vkTapN - 1];
            s_vkTapN--;
            continue;
        }
        int vk = s_vkTaps[i].vk;
        m[vk >> 6] |= 1ull << (vk & 63);
    }
    for (int w = 0; w < 4; w++)
        s_vkForce[w].store(m[w], std::memory_order_relaxed);
}

/* Walk the game's VK -> InputState-index std::map (head pointer at
 * exe+0x114AB930; MSVC _Tree node: _Left@0 _Parent@8 _Right@0x10
 * _Isnil@0x19, pair{vk@0x1C, idx@0x20}) and log every mapping. */
static void DumpVkMap() {
    __try {
        uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
        char* head = *(char**)(base + 0x114AB930);
        if (!head) { WriteLog("[MP] vkmap: no tree\n"); return; }
        auto isnil = [](char* n) { return n == nullptr || n[0x19] != 0; };
        int n = 0;
        char line[1024]; int off = 0;
        for (char* cur = *(char**)head; cur && cur != head; ) {
            int vk = *(int*)(cur + 0x1c);
            int idx = *(int*)(cur + 0x20);
            off += snprintf(line + off, sizeof(line) - off, "%s%d->%d",
                            (n % 12) ? "," : (n ? "\n[MP] vkmap " : ""), vk, idx);
            n++;
            /* successor */
            char* r = *(char**)(cur + 0x10);
            if (!isnil(r)) {
                cur = r;
                while (!isnil(*(char**)cur)) cur = *(char**)cur;
            } else {
                char* p = *(char**)(cur + 8);
                while (!isnil(p) && cur == *(char**)(p + 0x10)) {
                    cur = p; p = *(char**)(p + 8);
                }
                cur = p;
            }
            if (n > 400) break;
        }
        WriteLog("[MP] vkmap %s\n(%d entries)\n", line, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WriteLog("[MP] vkmap: walk faulted\n");
    }
}

void InstallGaksHook() {
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    auto** slot = (SHORT(WINAPI**)(int))(base + 0xB257F8);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        WriteLog("[MP] gaks: VirtualProtect failed %lu\n", GetLastError());
        return;
    }
    s_gaksReal = *slot;
    *slot = &GaksDetour;
    VirtualProtect(slot, sizeof(*slot), old, &old);
    WriteLog("[MP] gaks hook installed slot=%p orig=%p (user32=%p)\n",
             (void*)slot, (void*)s_gaksReal,
             (void*)GetProcAddress(GetModuleHandleW(L"user32.dll"),
"GetAsyncKeyState"));
    DumpVkMap();
}
#else
void VkTap(int, uint32_t) {}
void InstallGaksHook() {}
#endif

/* ---------------- device-bitset injection (post-VK-map) ----------------
 * The keyboard poller (exe+0x2B0CF0) rebuilds the input object's raw key
 * bitset at +0x518 every frame: clear, then set each index whose VK maps
 * to it and is physically held. The caller diffs old-vs-new to derive
 * edges. OR-ing our indices into +0x518 right after the original poll —
 * inside this hook, before the caller's diff — produces genuine
 * edge/down state through the game's own pipeline: no OS input, works
 * unfocused, and stays valid under user key remaps because +0x518 holds
 * the game's mapped action indices rather than scancodes. */
static SafetyHookInline s_kbPollHook;
static std::mutex     s_devMtx;
struct DevTapEntry { int32_t idx; uint64_t endTick; };
static DevTapEntry  s_devTaps[8];
static int          s_devTapN = 0;
static int          s_devTapSeen = 0;

static __int64 __fastcall KbPollDetour(uintptr_t obj) {
    __int64 r = s_kbPollHook.call<__int64>(obj);
    /* The poller reads the global key state, which includes keys typed
     * into other apps. Vanilla relies on the game pausing itself when the
     * window loses focus; in a room the song keeps running, so drop the
     * physical keyboard whenever the game window isn't foreground. */
    if (!GameForeground()) {
        uint64_t* bs = (uint64_t*)(obj + 0x518);
        bs[0] = bs[1] = bs[2] = 0;
    }
    uint64_t bits[3] = {};
    {   std::lock_guard<std::mutex> lk(s_devMtx);
        uint64_t now = NowTick();
        for (int i = s_devTapN - 1; i >= 0; i--) {
            if (now >= s_devTaps[i].endTick) {
                s_devTaps[i] = s_devTaps[s_devTapN - 1];
                s_devTapN--;
            } else {
                int idx = s_devTaps[i].idx;
                if (idx >= 0 && idx < 192) bits[idx >> 6] |= 1ull << (idx & 63);
            }
        }
    }
    if (bits[0] | bits[1] | bits[2]) {
        uint64_t* bs = (uint64_t*)(obj + 0x518);
        bs[0] |= bits[0]; bs[1] |= bits[1]; bs[2] |= bits[2];
        if (s_devTapSeen++ < 4)
            WriteLog("[MP] devtap: injected into obj=%p bits=%llx,%llx,%llx\n",
                     (void*)obj, (unsigned long long)bits[0],
                     (unsigned long long)bits[1], (unsigned long long)bits[2]);
    }
    return r;
}

/* Logical bits our own device-layer injection produces (device idx ==
 * InputState key idx). These must NOT feed s_realEdges — an injected
 * DevTap(28) otherwise looks like a physical Esc and the lobby's
 * ConsumeRealPress(Plus) answers our own toggle with pause_reqs
 * (pause<->resume ping-pong observed live 2026-11-18). */
static uint64_t DevTapSuppressLogical() {
    uint64_t supp = 0;
    std::lock_guard<std::mutex> lk(s_devMtx);
    uint64_t now = NowTick(), tail = MsToTick(80);
    for (int i = 0; i < s_devTapN; i++)
        if (now < s_devTaps[i].endTick + tail)
            supp |= keys::LogicalForKey(s_devTaps[i].idx);
    return supp;
}

/* dbg_cmd devtap {idx,hold}: hold raw device index <idx> in the keyboard
 * bitset for hold_ms — e.g. 28 = the Esc/pause entry's index. */
void DevTap(int32_t idx, uint32_t hold_ms) {
    if (idx < 0 || idx >= 192) return;
    std::lock_guard<std::mutex> lk(s_devMtx);
    for (int i = 0; i < s_devTapN; i++) {
        if (s_devTaps[i].idx == idx) {
            s_devTaps[i].endTick = NowTick() + MsToTick(hold_ms);
            return;
        }
    }
    if (s_devTapN < 8) {
        s_devTaps[s_devTapN].idx = idx;
        s_devTaps[s_devTapN].endTick = NowTick() + MsToTick(hold_ms);
        s_devTapN++;
    }
}

void DevTapBtn(uint64_t logical, uint32_t hold_ms) {
    int32_t idx = -1;
    switch (logical) {
    case nn::hid::A:     idx = keys::RawEnter; break;
    case nn::hid::B:     idx = keys::RawEsc;   break;
    case nn::hid::Up:    idx = keys::RawUp;    break;
    case nn::hid::Down:  idx = keys::RawDown;  break;
    case nn::hid::Left:  idx = keys::RawLeft;  break;
    case nn::hid::Right: idx = keys::RawRight; break;
    default: return;
    }
    DevTap(idx, hold_ms);
}

void InstallKbPollHook() {
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    s_kbPollHook = safetyhook::create_inline((void*)(base + 0x2B0CF0),
                                             (void*)&KbPollDetour);
    WriteLog("[MP] kbpoll hook %s @%p\n", s_kbPollHook ? "installed" : "FAILED",
             (void*)(base + 0x2B0CF0));
}

/* ======================= native pause control ==========================
 * MM+ 1.03 pause architecture (RE'd 2026-11-18, all on the game thread):
 *
 *  PauseManager task  B = exe+0xCC0CA90   (interface I = B+0x68, vtable
 *                                           exe+0xCC2548)
 *    B+0x70  state: 0 idle, 1 pause menu up, 2/3/4/5 special modes
 *    B+0x74  how the last pause ended (read by the pv update)
 *    B+0x530 pause wanted, B+0x531 system pause request,
 *    B+0x536 accepts requests (cleared while a pause is up)
 *    Ctrl exe+0x65BE90: in state 0 a pause starts when B+0x531 is set or
 *    the abstract pause action (InputState key 0xA0) is tapped; it spawns
 *    the pause-menu task and waits in state 1 for its result.
 *  I->vt[0x38] = exe+0x65C780 RequestSystemPause(I, bool): what the PV task
 *    calls every frame while the window is not foreground (exe+0x2684A8).
 *    Sets B+0x531 when the manager accepts requests.
 *  Pause-menu task    M = B+0x80, Ctrl exe+0x659ED0 returns bool (true =
 *    finished; the task runner then removes it).
 *    M+0x6C state (3 = idle/polls input, 5 = closing), M+0x70 result,
 *    M+0x74 has-result, M+0x75 resume flag, M+0x278 cursor
 *    (0 Resume 1 Retry 2 Lag Config 3 Options 4 Return to Song List).
 *  pv update exe+0x241DF0 (int, nonzero = song over): pauses when
 *    I->IsPaused(), and once the manager is back in state 0 reads the
 *    result (0 resume / 1 retry / 2 return to list).
 *  exe+0x14AB9E8: byte, 1 = do NOT pause when the window loses focus
 *    (only reader exe+0x2B8120, called from the PV task's focus check).
 *
 * So: pause = RequestSystemPause (the game's own call); resume = run the
 * menu's own "pause key pressed" branch from inside its Ctrl. No input
 * simulation, no key identity, works unfocused. Every detour returns the
 * original's value - both Ctrl and the pv update signal completion through
 * it (a swallowed return made the pause menu immortal and ended songs at
 * random). */
static SafetyHookInline        s_pauseTaskHook;
static std::atomic<uintptr_t>  s_pauseTask{0};
static std::atomic<uint64_t>   s_pauseTaskMs{0};
static std::atomic<int>        s_pauseWant{-1};      /* -1 none, 0 run, 1 pause */
static std::atomic<uint64_t>   s_pauseWantUntil{0};  /* ms; 0 = no expiry */
static std::atomic<int>        s_resumeFired{0};

static inline uintptr_t ExeBase() {
    static uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr);
    return b;
}
static inline volatile unsigned char* PvBytes() {
    return (volatile unsigned char*)(ExeBase() + 0x12C2330);
}
static inline uintptr_t PauseMgr() { return ExeBase() + 0xCC0CA90; }

static int CurrentPauseWant() {
    int w = s_pauseWant.load(std::memory_order_relaxed);
    if (w < 0) return -1;
    uint64_t until = s_pauseWantUntil.load(std::memory_order_relaxed);
    if (until && NowMs() > until) {
        s_pauseWant.store(-1, std::memory_order_relaxed);
        return -1;
    }
    return w;
}

/* Pause-menu Ctrl. When a resume is wanted and the menu is idle, take the
 * same branch the menu takes for the pause key (exe+0x65A067): resume
 * flag, closing state, close SE, out animations. */
static bool __fastcall PauseMenuCtrlDetour(uintptr_t task) {
    s_pauseTask.store(task, std::memory_order_relaxed);
    s_pauseTaskMs.store(NowMs(), std::memory_order_relaxed);
    if (CurrentPauseWant() == 0 && *(volatile int*)(task + 0x6c) == 3) {
        uintptr_t base = ExeBase();
        *(volatile unsigned char*)(task + 0x75) = 1;
        *(volatile int*)(task + 0x6c) = 5;
        ((int (__fastcall*)(int, const char*, float))(base + 0x5AA550))(
            1, "se_ft_sys_dialog_close", 1.0f);
        ((void (__fastcall*)(uintptr_t, int))(base + 0x657770))(task, 1);
        ((void (__fastcall*)(uintptr_t, int))(base + 0x656EB0))(task, 1);
        s_resumeFired.fetch_add(1, std::memory_order_relaxed);
        /* a one-shot is done once issued (a persistent target stays) */
        if (s_pauseWantUntil.load(std::memory_order_relaxed))
            s_pauseWant.store(-1, std::memory_order_relaxed);
        WriteLog("[MP] pause: menu resume issued\n");
    }
    return s_pauseTaskHook.call<bool>(task);
}

void InstallPauseTaskHook() {
    uintptr_t base = ExeBase();
    s_pauseTaskHook = safetyhook::create_inline((void*)(base + 0x659ED0),
                                                (void*)&PauseMenuCtrlDetour);
    WriteLog("[MP] pause-menu hook %s @%p\n",
             s_pauseTaskHook ? "installed" : "FAILED", (void*)(base + 0x659ED0));
}

static bool PauseMenuLive() {
    uintptr_t t = s_pauseTask.load(std::memory_order_relaxed);
    return t && NowMs() - s_pauseTaskMs.load(std::memory_order_relaxed) <= 250;
}

/* -1 when no pause-menu task has run recently (menu closed). */
int PauseMenuCursor() {
    if (!PauseMenuLive()) return -1;
    return *(volatile int*)(s_pauseTask.load(std::memory_order_relaxed) + 0x278);
}

void PauseMenuSetCursor(int c) {
    if (PauseMenuLive())
        *(volatile int*)(s_pauseTask.load(std::memory_order_relaxed) + 0x278) = c;
}

int PauseMenuTaskState() {
    if (!PauseMenuLive()) return -1;
    return *(volatile int*)(s_pauseTask.load(std::memory_order_relaxed) + 0x6c);
}

bool GamePaused() {
    volatile unsigned char* pv = PvBytes();
    return pv[0] && pv[1];
}

/* Idempotent pause target. want: 1 = paused, 0 = running, -1 = leave the
 * game alone. hold_ms > 0 makes it a one-shot that expires. PauseTick()
 * works towards it every pv frame; nothing here toggles, so repeating a
 * request can never undo it. */
void PauseWant(int want, uint32_t hold_ms) {
    s_pauseWantUntil.store(hold_ms ? NowMs() + hold_ms : 0,
                           std::memory_order_relaxed);
    s_pauseWant.store(want, std::memory_order_relaxed);
}

/* Drop a persistent target; a pending one-shot keeps running. */
void PauseWantRelease() {
    if (!s_pauseWantUntil.load(std::memory_order_relaxed))
        s_pauseWant.store(-1, std::memory_order_relaxed);
}

/* Game thread, once per pv frame (from the pv update detour). */
static void PauseTick() {
    int want = CurrentPauseWant();
    if (want != 1) return;            /* resume is done inside the menu Ctrl */
    volatile unsigned char* pv = PvBytes();
    if (pv[0] && pv[1] && s_pauseWantUntil.load(std::memory_order_relaxed))
        s_pauseWant.store(-1, std::memory_order_relaxed);   /* one-shot done */
    if (!pv[0] || pv[1]) return;      /* not in a song, or already paused */
    uintptr_t mgr = PauseMgr();
    if (*(volatile int*)(mgr + 0x70) != 0) return;   /* a pause is underway */
    /* the game's own request; ignored until the manager accepts requests */
    ((void (__fastcall*)(uintptr_t, uint8_t))(ExeBase() + 0x65C780))(
        mgr + 0x68, 0);
}

/* The game pauses the song every frame its window is not foreground. In a
 * multiplayer room that would desync the round (and the room would see a
 * pause nobody asked for), so it is switched off while in a room. */
static int s_bgPauseOrig = -1;
void SetBackgroundPause(bool gameDefault) {
    volatile unsigned char* f =
        (volatile unsigned char*)(ExeBase() + 0x14AB9E8);
    if (s_bgPauseOrig < 0) s_bgPauseOrig = *f;
    unsigned char v = gameDefault ? (unsigned char)s_bgPauseOrig : 1;
    if (*f != v) {
        *f = v;
        WriteLog("[MP] background pause %s\n",
                 gameDefault ? "restored (game default)" : "suppressed (in room)");
    }
}

/* MP start gate: the song is held at its first frame until the server's
 * `start`. PVGameLoaderFinishUp (exe+0x262730) tells us the load is done
 * (-> `loaded`); while mp::lobby::LoaderHeld() the pv update is skipped, so
 * clock, audio and notes don't start but the scene keeps rendering.
 * exe+0x241DF0 is `int PvUpdate(pv*, float dt, ctx*)`; its caller
 * (exe+0x2684C9) ends the song when it returns nonzero, so the detour
 * returns the original's value, and 0 while holding. */
static SafetyHookInline s_loaderHook;
static SafetyHookInline s_pvUpdateHook;

static bool __fastcall LoaderFinishUpDetour(void* a1) {
    bool done = s_loaderHook.call<bool>(a1);
    if (done) mp::lobby::LoaderFinished();
    return done;
}

/* dbg sprtest: a translucent red rect over the clear gauge drawn through
 * the game's own sprite queue at priority g_sprTestPrio (720p coords) */
int g_sprTestPrio = -1;
void SprTestDraw() {
    if (g_sprTestPrio < 0) return;
    alignas(16) static uint8_t a[0x200];
    memset(a, 0, sizeof(a));
    ((void* (__fastcall*)(void*))(ExeBase() + 0x5B78D0))(a);   /* DefaultSprArgs */
    *(uint32_t*)(a + 0x00) = 4;                                 /* SPR_KIND_MULTI */
    a[0x08] = 40; a[0x09] = 40; a[0x0A] = 255; a[0x0B] = 200;   /* BGRA red */
    *(uint32_t*)(a + 0x0C) |= 0x08000000;                       /* SPR_ATTR_MATRIX */
    *(int32_t*)(a + 0x1C) = g_sprTestPrio;
    *(int32_t*)(a + 0x20) = 13; *(int32_t*)(a + 0x24) = 13;     /* HD 1280x720 */
    float* tr = (float*)(a + 0x34); tr[0] = 130.0f; tr[1] = 690.0f; tr[2] = 0.0f;
    *(uint32_t*)(a + 0x12C) |= 1;                               /* SPRITE_SIZE */
    float* sz = (float*)(a + 0x130); sz[0] = 1100.0f; sz[1] = 24.0f;
    ((void* (__fastcall*)(void*))(ExeBase() + 0x5B49C0))(a);   /* put_sprite */
}

static int __fastcall PvUpdateDetour(void* a1, float a2, void* a3) {
    static int s_updHeld = 0;
    if (mp::lobby::LoaderHeld()) {
        if (!(++s_updHeld % 120))
            WriteLog("[MP] pv-update held x%d\n", s_updHeld);
        return 0;
    }
    PauseTick();
    return s_pvUpdateHook.call<int>(a1, a2, a3);
}

void InstallLoaderHook() {
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    s_loaderHook = safetyhook::create_inline((void*)(base + 0x262730),
                                             (void*)&LoaderFinishUpDetour);
    s_pvUpdateHook = safetyhook::create_inline((void*)(base + 0x241DF0),
                                               (void*)&PvUpdateDetour);
    WriteLog("[MP] loader hook %s @%p / pv-update hook %s @%p\n",
             s_loaderHook ? "installed" : "FAILED", (void*)(base + 0x262730),
             s_pvUpdateHook ? "installed" : "FAILED", (void*)(base + 0x241DF0));
}

#ifdef MP_DEV_TOOLS
/* ---------------- Tapped() caller trace (menu-dispatch RE) --------------
 * IsButtonTapped (exe+0x2AAF80, rcx=InputState edx=key, reads bitset+0x00)
 * is queried by menu/scene code via the delegate thunk 0x1402AB260 ->
 * CheckButtonDelegate 0x1402AB1A0. While `tracetap` is on we log key, the
 * InputState and up-stack return addresses for nav keys 3-6 — the real
 * caller is 1-2 frames above the delegate. */
static SafetyHookInline s_tappedHook;
static std::atomic<int> s_traceTap{0};
void SetTraceTap(int on) { s_traceTap.store(on); }

/* Hook CheckButtonDelegate (exe+0x2AB1A0): rcx=InputState edx=key r8=bound
 * accessor (Tapped 0x1402AAF80 / Repeat 0x1402AB070 / Down 0x1402AAFE0 /
 * repB 0x1402AB040). Its return address is the real consumer call site. */
static __int64 __fastcall TappedDetour(uintptr_t st, int32_t key, uintptr_t acc) {
    __int64 r = s_tappedHook.call<__int64>(st, key, acc);
    if (s_traceTap.load()) {
        uintptr_t* fp = (uintptr_t*)_AddressOfReturnAddress();
        uintptr_t rets[6]; int n = 0;
        __try {
            for (int i = 0; i < 0x30 && n < 6; i++) {
                uintptr_t v = fp[i];
                if (v >= 0x140001000 && v < 0x140b25000) rets[n++] = v;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        char line[320]; int o = snprintf(line, sizeof line,
            "[MP] tracetap st=%llx key=%d acc=%llx",
            (unsigned long long)st, key, (unsigned long long)acc);
        for (int i = 0; i < n; i++)
            o += snprintf(line + o, sizeof line - o, " %llx",
                          (unsigned long long)rets[i]);
        o += snprintf(line + o, sizeof line - o, "\n");
        KLog("%s", line);
    }
    return r;
}

void InstallTappedHook() {
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    s_tappedHook = safetyhook::create_inline((void*)(base + 0x2AB1A0),
                                             (void*)&TappedDetour);
    WriteLog("[MP] btndel hook %s @%p\n", s_tappedHook ? "installed" : "FAILED",
             (void*)(base + 0x2AB1A0));
}
#else
void SetTraceTap(int) {}
void InstallTappedHook() {}
#endif

/* ---------------- per-InputState bookkeeping ---------------- */
struct Rec {
    int      slot = -1;        /* 0..26 for edge latching / menu::Filter */
    uint64_t prevFilt = 0;     /* filtered logical buttons last frame */
    uint64_t prevTap[3] = {};  /* keylog: tapped words seen last Apply */
    uint64_t prevDown[3] = {}; /* keylog: down words seen last Apply */
    uint64_t merged = 0;       /* real|inject logical (nav) */
    uint64_t seenMs = 0;
    int32_t  stickL[2] = {};
    int32_t  stickR[2] = {};
    /* last computed write-state, re-applied after the game's per-frame
     * clear in SetInputNotDispatchable (focus/block gates wipe the live
     * bitsets right after Apply runs) */
    uint64_t filt = 0, edges = 0, blocked = 0, realRepeat[3] = {};
    uint64_t injFilt = 0, injEdges = 0;   /* the mod's own presses only */
    bool     blockAll = false, zeroLs = false;
};
static std::map<diva::InputState*, Rec> s_recs;
static int s_nextSlot = 0;

static inline bool BitTest(const uint64_t* w, int key) {
    return (w[key >> 6] >> (key & 63)) & 1;
}
static inline void BitSet(uint64_t* w, int key, bool v) {
    if (v) w[key >> 6] |= (1ull << (key & 63));
    else   w[key >> 6] &= ~(1ull << (key & 63));
}

/* logical -> keys table snapshot for Apply (immutable) */
static int KeyBitCount() { return (int)addr::InputKeyCount; }

/* Compute the pending inject masks (same rules as the Switch Apply). */
static void CollectInject(uint64_t now, uint64_t* inject, uint64_t* injectForce) {
    if (s_spam.active && s_spam.hz > 0) {
        if (now >= s_spam.endTick) {
            s_spam.active = false;
        } else {
            uint64_t period = nn::os::GetSystemTickFrequency() / s_spam.hz;
            if (period > 0 && (now - s_spam.startTick) % period < period / 2)
                *inject |= s_spam.buttons;
        }
    }
    while (s_tapCount > 0) {
        const TapEntry& e = s_taps[s_tapHead];
        uint64_t elapsed = now - s_tapStartTick;
        if (elapsed < MsToTick(e.hold_ms)) {
            if (e.force) *injectForce |= e.buttons;
            else         *inject |= e.buttons;
            break;
        }
        if (elapsed < MsToTick(e.hold_ms + e.gap_ms))
            break;                       /* release window */
        s_tapHead = (s_tapHead + 1) & 31;
        s_tapCount--;
        s_tapStartTick = now;
    }
}

static RawTapEntry s_rawTapSnap[8];
static int s_rawTapSnapN = 0;

/* Write the filtered/injected logical state into st's key bitsets.
 * Called twice per frame per state: once in Apply (so the game's
 * not-dispatchable snapshot copies injected bits like real input) and
 * once after SetInputNotDispatchable (which may clear the live sets). */
static void WriteSets(diva::InputState* st, uint64_t filt, uint64_t edges,
                      uint64_t blocked, bool blockAll, bool zeroLs,
                      const uint64_t* realRepeat,
                      const RawTapEntry* rawTaps, int rawN) {
    (void)zeroLs;
    uint64_t* tW  = st->KeysTapped();
    uint64_t* t2W = st->KeysAt(addr::InputOffTapped2);
    uint64_t* dW  = st->KeysDown();
    uint64_t* rW  = st->KeysRepeat();
    uint64_t* u8W = st->KeysAt(addr::InputOffUnk0A8);
    uint64_t* aW  = st->KeysAbsTapped();

    auto applyIdx = [&](int k, bool down, bool edge) {
        if (!down) {
            BitSet(tW, k, false);
            BitSet(t2W, k, false);
            BitSet(dW, k, false);
            BitSet(rW, k, false);
            BitSet(u8W, k, false);
            BitSet(aW, k, false);
        } else {
            BitSet(dW, k, true);
            BitSet(tW, k, edge);
            BitSet(t2W, k, edge);
            BitSet(u8W, k, true);
            BitSet(aW, k, edge);
            bool rep = BitTest(realRepeat, k) || edge;
            BitSet(rW, k, rep);
        }
    };
    /* Logical bits driving each key index: the primary mapping plus every
     * signature that lists it. Shared indices (e.g. the any-key index,
     * set by A/B/Plus alike) must OR all contributors - writing them per
     * bit lets a later bit's absent state clobber an earlier injected
     * one, which ate injected input whenever blocks were active. */
    uint64_t contrib[addr::InputKeyCount];
    for (int k = 0; k < KeyBitCount(); k++)
        contrib[k] = keys::LogicalForKey(k);
    for (int i = 0; i < 64; i++) {
        uint64_t bb = 1ull << i;
        if (!(filt & bb) && !(blocked & bb)) continue;
        int32_t sig[8];
        int n = keys::KeySignature(bb, sig, 8);
        for (int j = 0; j < n; j++)
            contrib[sig[j]] |= bb;
    }
    for (int k = 0; k < KeyBitCount(); k++) {
        if (!contrib[k]) continue;
        applyIdx(k, (filt & contrib[k]) != 0, (edges & contrib[k]) != 0);
    }

    /* raw-index taps (discovery): write every press bitset directly */
    for (int i = 0; i < rawN; i++) {
        int k = rawTaps[i].idx;
        BitSet(tW, k, true);
        BitSet(t2W, k, true);
        BitSet(dW, k, true);
        BitSet(rW, k, true);
        BitSet(u8W, k, true);
        BitSet(aW, k, true);
    }
}

/* Called by the SetInputNotDispatchable hook, post-original: the game may
 * have just cleared the live bitsets (dispatch gate / unfocused input
 * drop), so re-write the state Apply computed this frame. */
void ApplyPost(diva::InputState* st, bool notDispatchable) {
    auto it = s_recs.find(st);
    if (it == s_recs.end()) return;
    Rec& rec = it->second;
    if (NowMs() - rec.seenMs > 250) return;          /* stale */
    static std::map<diva::InputState*, int> s_lastFlag;
    int f = notDispatchable ? 1 : 0;
    if (s_lastFlag[st] != f) {
        s_lastFlag[st] = f;
        WriteLog("[MP] notdisp st=%p flag=%d\n", (void*)st, f);
    }
    /* when not-dispatchable the game already snapshotted our Apply-time
     * live writes into the abs set; re-writing live sets makes injected
     * input reach normal readers too (synthetic input isn't subject to
     * the scene's own block - our block mask already ran through filt) */
    /* Unfocused, the game wipes the live sets every frame: put back only
     * the mod's own presses. Re-writing the real keys here undid the
     * game's focus gate and let keys typed into other apps reach it.
     * Focused, re-apply the full state only when the scene withheld input
     * (modal gate), as before. */
    if (!GameForeground())
        WriteSets(st, rec.injFilt, rec.injEdges, rec.blocked, rec.blockAll,
                  false, rec.realRepeat, s_rawTapSnap, s_rawTapSnapN);
    else if (notDispatchable)
        WriteSets(st, rec.filt, rec.edges, rec.blocked, rec.blockAll, false,
                  rec.realRepeat, s_rawTapSnap, s_rawTapSnapN);
}

/* Called by the PollInputRepeatAndDouble hook, post-original, once per
 * InputState per frame. Applies blocks/injections by editing the key
 * bitsets; keeps edge latching, keylog, nav merge and menu::Filter. */
void Apply(diva::InputState* st) {
    if (!st) return;
    uint64_t now = NowTick();
    uint64_t nowMs = NowMs();

    auto it = s_recs.find(st);
    if (it == s_recs.end()) {
        Rec r;
        r.slot = s_nextSlot < 27 ? s_nextSlot++ : 26;
        it = s_recs.emplace(st, r).first;
    }
    Rec& rec = it->second;
    const int slot = rec.slot;

    /* --- real logical buttons (pre-block) --- */
    uint64_t real = 0;
    for (int k = 0; k < KeyBitCount(); k++)
        if (BitTest(st->KeysDown(), k))
            real |= keys::LogicalForKey(k);

    uint64_t inject = 0, injectForce = 0, blocked = 0;
    bool zeroLs = false;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_bypassDepth > 0 && s_bypassTid == CurTid()) {
            rec.merged = real; rec.seenMs = nowMs;
            return;                      /* our own poll sees real input */
        }
        s_srcReal[slot] = real;
        s_srcRealMs[slot] = nowMs;
        uint64_t comb = 0;
        for (int i = 0; i < 27; i++)
            if (nowMs - s_srcRealMs[i] <= 250) comb |= s_srcReal[i];
        comb &= ~DevTapSuppressLogical();
        s_realEdges |= comb & ~s_prevRealComb;
        s_prevRealComb = comb;
        /* DevTap-produced logicals are the mod's own injection: they must
         * pass the block mask like injectForce does, or the results/pause
         * drivers can't reach menus while the block owns input (the menus
         * read the device-driven path, where Tap(force) never lands). */
        blocked = s_blocked & ~DevTapSuppressLogical();
        zeroLs = s_zeroSticks;
        CollectInject(now, &inject, &injectForce);
#ifdef MP_DEV_TOOLS
        for (int i = s_rawTapN - 1; i >= 0; i--) {
            if (now >= s_rawTaps[i].endTick) {
                s_rawTaps[i] = s_rawTaps[s_rawTapN - 1];
                s_rawTapN--;
            }
        }
        s_rawTapSnapN = s_rawTapN;
        memcpy(s_rawTapSnap, s_rawTaps, sizeof(s_rawTaps));
        s_diForce = inject | injectForce;
        s_diBlock = blocked;
        UpdateVkForce(inject, injectForce, now);
#endif
    }
#ifdef MP_DEV_TOOLS
    s_diBlockAll = (blocked & ~nn::hid::Minus) == ~nn::hid::Minus;
#endif

    /* keylog: every bitset offset on the state being polled. Buffered
     * (KLog) so no mutex/file I/O happens per transition line. */
#ifdef MP_DEV_TOOLS
    if (s_keylog.load()) {
        static const size_t kOffs[] = {
            addr::InputOffTapped,  addr::InputOffTapped2,
            addr::InputOffDown,    addr::InputOffPrevDown,
            addr::InputOffRepA,    addr::InputOffRepB,
            addr::InputOffRepeat,  addr::InputOffUnk0A8,
            addr::InputOffUnk0C0,  addr::InputOffAbsTapped,
        };
        static const char* kNames[] = {
            "tap", "tap2", "down", "prev", "repA",
"repB", "rep",  "u0a8", "u0c0", "abs",
        };
        enum { NSET = 10 };
        static uint64_t s_prev[8][NSET][3] = {};
        for (int s2 = 0; s2 < NSET; s2++) {
            uint64_t* cur = st->KeysAt(kOffs[s2]);
            int si = slot < 8 ? slot : 0;
            for (int w = 0; w < 3; w++) {
                uint64_t diff = cur[w] ^ s_prev[si][s2][w];
                while (diff) {
                    unsigned long bi = 0;
                    _BitScanForward64(&bi, diff);
                    int key = w * 64 + (int)bi;
                    bool v = (cur[w] >> bi) & 1;
                    KLog("[MP] keylog t=%llu slot=%d set=%s key=%d %s\n",
                         (unsigned long long)GetTickCount64(),
                         si, kNames[s2], key, v ? "1" : "0");
                    diff &= diff - 1;
                }
                s_prev[si][s2][w] = cur[w];
            }
        }
    }
#endif
    KLogFlush();

    /* --- effective logical down per key --- */
    bool blockAll = (blocked & ~nn::hid::Minus) == ~nn::hid::Minus;
    uint64_t merged = real | inject | injectForce;   /* nav sees pre-block */
    uint64_t eff = merged & ~blocked | injectForce;  /* force beats block */
    if (blockAll) eff = injectForce;                 /* full block: injects also masked unless force */

    /* menu model on the filtered stream (post-block, pre-write) */
    uint64_t filt = eff;
    int32_t stickL[2], stickR[2];
    int32_t* pos = st->Positions();
    stickL[0] = pos[addr::InputPosStickBase + 0];
    stickL[1] = pos[addr::InputPosStickBase + 1];
    stickR[0] = pos[addr::InputPosStickBase + 2];
    stickR[1] = pos[addr::InputPosStickBase + 3];
    menu::Filter(slot, &filt, stickL, stickR, nowMs);

    uint64_t changed = filt ^ rec.prevFilt;
    uint64_t edges = filt & ~rec.prevFilt;           /* rising edges only */
    (void)changed;
    rec.prevFilt = filt;

    /* stash the write-state: the SetInputNotDispatchable hook re-applies
     * it after the game clears the live bitsets (unfocused/dispatch gate) */
    rec.filt = filt; rec.edges = edges; rec.blocked = blocked;
    rec.injFilt = filt & (inject | injectForce);
    rec.injEdges = edges & (inject | injectForce);
    rec.blockAll = blockAll; rec.zeroLs = zeroLs;
    memcpy(rec.realRepeat, st->KeysRepeat(), sizeof(rec.realRepeat));

    WriteSets(st, filt, edges, blocked, blockAll, zeroLs,
              rec.realRepeat, s_rawTapSnap, s_rawTapSnapN);

    if (zeroLs) {
        pos[addr::InputPosStickBase + 0] = 0;
        pos[addr::InputPosStickBase + 1] = 0;
        pos[addr::InputPosStickBase + 2] = 0;
        pos[addr::InputPosStickBase + 3] = 0;
    }

    rec.merged = merged;
    rec.seenMs = nowMs;
    memcpy(rec.stickL, stickL, sizeof(stickL));
    memcpy(rec.stickR, stickR, sizeof(stickR));
}

/* ---------------- merged state + emergency exit ---------------- */
static nn::hid::NpadHandheldState s_merged;
static std::mutex s_mergeMtx;
static uint64_t s_f9Since = 0;
static uint64_t s_backSince = 0;
static bool     s_emergDone = false;

static bool GameForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == GetConsoleWindow()) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static uint64_t CurrentInjectBits() {
    std::lock_guard<std::mutex> lk(s_mtx);
    uint64_t inj = 0, injF = 0;
    /* non-destructive peek: taps+spam are per-frame; reconstruct via the
     * same logic without advancing the queue */
    uint64_t now = NowTick();
    if (s_spam.active && s_spam.hz > 0 && now < s_spam.endTick) {
        uint64_t period = nn::os::GetSystemTickFrequency() / s_spam.hz;
        if (period > 0 && (now - s_spam.startTick) % period < period / 2)
            inj |= s_spam.buttons;
    }
    if (s_tapCount > 0) {
        const TapEntry& e = s_taps[s_tapHead];
        uint64_t elapsed = now - s_tapStartTick;
        if (elapsed < MsToTick(e.hold_ms)) {
            if (e.force) injF |= e.buttons;
            else         inj |= e.buttons;
        }
    }
    return inj | injF;
}

/* Per presented frame (mod.cpp): rebuild the merged logical state, feed nav
 * + check the emergency exit (F9 / pad Back held 3 s). */
void PollMerged() {
    uint64_t nowMs = NowMs();
    uint64_t b = CurrentInjectBits();
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        for (auto& [st, rec] : s_recs) {
            if (nowMs - rec.seenMs > 250) continue;
            b |= rec.merged;
            lx = rec.stickL[0]; ly = rec.stickL[1];
            rx = rec.stickR[0]; ry = rec.stickR[1];
        }
    }

    /* emergency: F9 or XInput Back held 3 s while the game is foreground */
    bool f9 = GameForeground() && (GetAsyncKeyState(VK_F9) & 0x8000);
    bool back = false;
    if (GameForeground()) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT && !back; i++) {
            XINPUT_STATE xs{};
            if (XInputGetState(i, &xs) == 0 &&
                (xs.Gamepad.wButtons & XINPUT_GAMEPAD_BACK))
                back = true;
        }
    }
    if (f9) {
        if (!s_f9Since) s_f9Since = nowMs;
        else if (nowMs - s_f9Since >= 3000 && !s_emergDone) {
            s_emergDone = true;
            ResetAll();
            NavClear();
            WriteLog("[MP] emergency exit: F9 held 3s, input reset\n");
        }
    } else s_f9Since = 0, s_emergDone = false;
    if (back) {
        if (!s_backSince) s_backSince = nowMs;
        else if (nowMs - s_backSince >= 3000 && !s_emergDone) {
            s_emergDone = true;
            ResetAll();
            NavClear();
            WriteLog("[MP] emergency exit: pad Back held 3s, input reset\n");
        }
    } else s_backSince = 0;
    if (f9 || back) b |= nn::hid::Minus;  /* Minus shows as held to readers */

    nn::hid::NpadHandheldState st;
    st.buttons = b;
    st.analogStickL[0] = lx; st.analogStickL[1] = ly;
    st.analogStickR[0] = rx; st.analogStickR[1] = ry;
    {
        std::lock_guard<std::mutex> lk(s_mergeMtx);
        s_merged = st;
    }
    NavUpdate(st);
}

} // namespace mp::input

namespace nn { namespace hid {
NpadHandheldState GetMergedNpadState() {
    std::lock_guard<std::mutex> lk(mp::input::s_mergeMtx);
    return mp::input::s_merged;
}
}} // namespace nn::hid

struct BtnName { const char* name; uint64_t bit; };
static const BtnName kBtns[] = {
    {"A", nn::hid::A}, {"B", nn::hid::B}, {"X", nn::hid::X}, {"Y", nn::hid::Y},
    {"L", nn::hid::L}, {"R", nn::hid::R}, {"ZL", nn::hid::ZL}, {"ZR", nn::hid::ZR},
    {"Plus", nn::hid::Plus}, {"Minus", nn::hid::Minus},
    {"Up", nn::hid::Up}, {"Down", nn::hid::Down},
    {"Left", nn::hid::Left}, {"Right", nn::hid::Right},
    {"LStick", nn::hid::LStick}, {"RStick", nn::hid::RStick},
};

namespace mp { namespace input {
uint64_t MaskFromNames(const char* names) {
    if (names == nullptr) return 0;
    uint64_t mask = 0;
    char buf[96];
    strncpy(buf, names, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* p = strtok(buf, ","); p; p = strtok(nullptr, ",")) {
        while (*p == ' ') p++;
        for (const auto& b : kBtns)
            if (strcmp(p, b.name) == 0) { mask |= b.bit; break; }
    }
    return mask;
}
}} // namespace mp::input
