#include "MpApi.hpp"
#include "../logger.hpp"
#include "lib.hpp"
#include <nn.hpp>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace mp {

/* ------------------------------------------------------------------ */
/* Symbol resolution (same pattern as MemoryHooks::ResolveSymbol)      */
/* ------------------------------------------------------------------ */

static uintptr_t ResolveSymbol(const char* sym) {
    for (int i = static_cast<int>(exl::util::ModuleIndex::Start);
         i < static_cast<int>(exl::util::ModuleIndex::End); i++) {
        auto index = static_cast<exl::util::ModuleIndex>(i);
        if (exl::util::HasModule(index)) {
            Elf_Sym* symbol = exl::reloc::GetSymbol(index, sym);
            if (symbol != nullptr) {
                return exl::util::GetModuleInfo(index).m_Total.m_Start + symbol->st_value;
            }
        }
    }
    return 0;
}

using FnInitPool      = uint32_t (*)(void* pool, uint64_t pool_size, uint64_t alloc_pool_size, int concurrency);
using FnInitConfig    = uint32_t (*)(const void* config);
using FnFinalize      = uint32_t (*)();
using FnSocket        = int (*)(int, int, int);
using FnConnect       = int (*)(int, const void*, uint32_t);
using FnSendRecv      = int64_t (*)(int, const void*, uint64_t, int);
using FnRecv          = int64_t (*)(int, void*, uint64_t, int);
using FnClose         = int (*)(int);
using FnPoll          = int (*)(MpPollFd*, uint64_t, int);
using FnFcntl         = int (*)(int, int, ...);
using FnSetSockOpt    = int (*)(int, int, int, const void*, uint32_t);
using FnBind          = int (*)(int, const void*, uint32_t);
using FnRecvFrom      = int64_t (*)(int, void*, uint64_t, int, void*, uint32_t*);
using FnSendTo        = int64_t (*)(int, const void*, uint64_t, int, const void*, uint32_t);
using FnInetPton      = int (*)(int, const char*, void*);
using FnInetHtons     = uint16_t (*)(uint16_t);
using FnGetLastErrno  = int (*)();

using FnNifmInit      = uint32_t (*)();
using FnNifmSubmit    = void (*)();
using FnNifmBool      = bool (*)();
using FnNifmGetIp     = uint32_t (*)(uint32_t*);

static FnInitPool     p_InitPool      = nullptr;
static FnInitConfig   p_InitConfig    = nullptr;
static FnFinalize     p_Finalize      = nullptr;
static FnSocket       p_Socket        = nullptr;
static FnConnect      p_Connect       = nullptr;
static FnSendRecv     p_Send          = nullptr;
static FnRecv         p_Recv          = nullptr;
static FnClose        p_Close         = nullptr;
static FnPoll         p_Poll          = nullptr;
static FnFcntl        p_Fcntl         = nullptr;
static FnSetSockOpt   p_SetSockOpt    = nullptr;
static FnBind         p_Bind          = nullptr;
static FnRecvFrom     p_RecvFrom      = nullptr;
static FnSendTo       p_SendTo        = nullptr;
static FnInetPton     p_InetPton      = nullptr;
static FnInetHtons    p_InetHtons     = nullptr;
static FnGetLastErrno p_GetLastErrno  = nullptr;

static FnNifmInit     p_NifmInit      = nullptr;
static FnNifmSubmit   p_NifmSubmit    = nullptr;
static FnNifmBool     p_NifmOnHold    = nullptr;
static FnNifmBool     p_NifmAvailable = nullptr;
static FnNifmGetIp    p_NifmGetIp     = nullptr;

static uintptr_t addr_InitPool   = 0;
static uintptr_t addr_InitConfig = 0;
static uintptr_t addr_Finalize   = 0;

static uintptr_t s_resolveSink = 0;

#define MP_RESOLVE(var, addr_var, sym) do {                                \
        uintptr_t a_ = ResolveSymbol(sym);                                 \
        if (a_ == 0) { WriteLog("[MP] missing symbol: %s\n", sym);         \
                       missing = true; }                                   \
        else { var = reinterpret_cast<decltype(var)>(a_); }                \
        *(addr_var) = a_;                                                  \
    } while (0)

bool ApiResolveAll() {
    bool missing = false;

    MP_RESOLVE(p_InitPool,   &addr_InitPool,   "_ZN2nn6socket10InitializeEPvmmi");
    MP_RESOLVE(p_InitConfig, &addr_InitConfig, "_ZN2nn6socket10InitializeERKNS0_6ConfigE");
    MP_RESOLVE(p_Finalize,   &addr_Finalize,   "_ZN2nn6socket8FinalizeEv");
    MP_RESOLVE(p_Socket, &s_resolveSink, "_ZN2nn6socket6SocketEiii");
    MP_RESOLVE(p_Connect, &s_resolveSink, "_ZN2nn6socket7ConnectEiPK8sockaddrj");
    MP_RESOLVE(p_Send, &s_resolveSink, "_ZN2nn6socket4SendEiPKvmi");
    MP_RESOLVE(p_Recv, &s_resolveSink, "_ZN2nn6socket4RecvEiPvmi");
    MP_RESOLVE(p_Close, &s_resolveSink, "_ZN2nn6socket5CloseEi");
    MP_RESOLVE(p_Poll, &s_resolveSink, "_ZN2nn6socket4PollEP6pollfdmi");
    MP_RESOLVE(p_Fcntl, &s_resolveSink, "_ZN2nn6socket5FcntlEiiz");
    MP_RESOLVE(p_SetSockOpt, &s_resolveSink, "_ZN2nn6socket10SetSockOptEiiiPKvj");
    MP_RESOLVE(p_Bind, &s_resolveSink, "_ZN2nn6socket4BindEiPK8sockaddrj");
    MP_RESOLVE(p_RecvFrom, &s_resolveSink, "_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj");
    MP_RESOLVE(p_SendTo, &s_resolveSink, "_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj");
    MP_RESOLVE(p_InetPton, &s_resolveSink, "_ZN2nn6socket8InetPtonEiPKcPv");
    MP_RESOLVE(p_InetHtons, &s_resolveSink, "_ZN2nn6socket9InetHtonsEt");
    MP_RESOLVE(p_GetLastErrno, &s_resolveSink,"_ZN2nn6socket12GetLastErrnoEv");

    MP_RESOLVE(p_NifmInit, &s_resolveSink, "_ZN2nn4nifm10InitializeEv");
    MP_RESOLVE(p_NifmSubmit, &s_resolveSink, "_ZN2nn4nifm20SubmitNetworkRequestEv");
    MP_RESOLVE(p_NifmOnHold, &s_resolveSink, "_ZN2nn4nifm22IsNetworkRequestOnHoldEv");
    MP_RESOLVE(p_NifmAvailable, &s_resolveSink, "_ZN2nn4nifm18IsNetworkAvailableEv");
    MP_RESOLVE(p_NifmGetIp, &s_resolveSink, "_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr");

    return !missing;
}

#undef MP_RESOLVE

/* ------------------------------------------------------------------ */
/* Initialize/Finalize guards                                          */
/* ------------------------------------------------------------------ */

static std::atomic<bool> g_socketInit{false};
static std::atomic<bool> g_mpEnabled{false};
static void* g_ourPool = nullptr;

bool IsSocketInitialized() { return g_socketInit.load(std::memory_order_acquire); }
void SetMpEnabled(bool enabled) { g_mpEnabled.store(enabled, std::memory_order_release); }

static void LogCaller(const char* what) {
    void* ra = __builtin_return_address(0);
    uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
    intptr_t off = reinterpret_cast<intptr_t>(ra) - static_cast<intptr_t>(base);
    WriteLog("[MP] %s caller=%p (main%c0x%llx)\n", what, ra,
             off >= 0 ? '+' : '-', off >= 0 ? (uint64_t)off : (uint64_t)(-off));
}

HOOK_DEFINE_TRAMPOLINE(SocketInitPoolGuard) {
    static uint32_t Callback(void* pool, uint64_t pool_size, uint64_t alloc_pool_size, int concurrency) {
        if (g_socketInit.load(std::memory_order_acquire)) {
            WriteLog("[MP] game socket Initialize skipped (already init)\n");
            return 0;
        }
        uint32_t res = Orig(pool, pool_size, alloc_pool_size, concurrency);
        WriteLog("[MP] socket Initialize(pool=%p size=0x%llx alloc=0x%llx conc=%d) -> 0x%x\n",
                 pool, pool_size, alloc_pool_size, concurrency, res);
        LogCaller("  Initialize");
        if (res == 0) g_socketInit.store(true, std::memory_order_release);
        return res;
    }
};

HOOK_DEFINE_TRAMPOLINE(SocketInitConfigGuard) {
    static uint32_t Callback(const void* config) {
        if (g_socketInit.load(std::memory_order_acquire)) {
            WriteLog("[MP] game socket Initialize(Config) skipped (already init)\n");
            return 0;
        }
        uint32_t res = Orig(config);
        WriteLog("[MP] socket Initialize(Config=%p) -> 0x%x\n", config, res);
        LogCaller("  Initialize(Config)");
        if (res == 0) g_socketInit.store(true, std::memory_order_release);
        return res;
    }
};

HOOK_DEFINE_TRAMPOLINE(SocketFinalizeGuard) {
    static uint32_t Callback() {
        if (g_mpEnabled.load(std::memory_order_acquire)) {
            WriteLog("[MP] game socket Finalize swallowed (mp active)\n");
            LogCaller("  Finalize");
            return 0;
        }
        uint32_t res = Orig();
        WriteLog("[MP] socket Finalize -> 0x%x\n", res);
        if (res == 0) g_socketInit.store(false, std::memory_order_release);
        return res;
    }
};

void InstallSocketGuards() {
    ApiResolveAll();
    if (addr_InitPool)
        SocketInitPoolGuard::InstallAtPtr(addr_InitPool);
    if (addr_InitConfig)
        SocketInitConfigGuard::InstallAtPtr(addr_InitConfig);
    if (addr_Finalize)
        SocketFinalizeGuard::InstallAtPtr(addr_Finalize);
    WriteLog("[MP] socket guards installed (init_pool=%p init_cfg=%p fin=%p)\n",
             (void*)addr_InitPool, (void*)addr_InitConfig, (void*)addr_Finalize);
}

bool EnsureSocketInit() {
    if (IsSocketInitialized())
        return true;
    if (p_InitPool == nullptr) {
        WriteLog("[MP] EnsureSocketInit: Initialize symbol unresolved\n");
        return false;
    }
    /* game's own params (never lower — its BerkeleySocketDriver threads
     * share the limit): 6MB pool, 0x20000 alloc pool, conc 14 */
    void* pool = aligned_alloc(0x1000, 0x600000);
    if (pool == nullptr) {
        WriteLog("[MP] EnsureSocketInit: pool alloc failed\n");
        return false;
    }
    /* Goes through the guard hook: it forwards to Orig and sets the flag. */
    uint32_t res = p_InitPool(pool, 0x600000, 0x20000, 0xe);
    if (res == 0) {
        g_ourPool = pool;
        g_socketInit.store(true, std::memory_order_release);
        WriteLog("[MP] EnsureSocketInit: ok (pool=%p)\n", pool);
        return true;
    }
    WriteLog("[MP] EnsureSocketInit failed, Result=0x%x\n", res);
    free(pool);
    return false;
}

/* ------------------------------------------------------------------ */
/* Wrappers                                                            */
/* ------------------------------------------------------------------ */

int SockSocket(int family, int type, int proto) {
    return p_Socket ? p_Socket(family, type, proto) : -1;
}
int SockConnect(int fd, const void* addr, uint32_t len) {
    return p_Connect ? p_Connect(fd, addr, len) : -1;
}
int64_t SockSend(int fd, const void* buf, uint64_t len, int flags) {
    return p_Send ? p_Send(fd, buf, len, flags) : -1;
}
int64_t SockRecv(int fd, void* buf, uint64_t len, int flags) {
    return p_Recv ? p_Recv(fd, buf, len, flags) : -1;
}
int SockClose(int fd) {
    return p_Close ? p_Close(fd) : -1;
}
int SockPoll(MpPollFd* fds, uint64_t nfds, int timeout_ms) {
    return p_Poll ? p_Poll(fds, nfds, timeout_ms) : -1;
}
int SockFcntl(int fd, int cmd, int arg) {
    int r = p_Fcntl ? p_Fcntl(fd, cmd, arg) : -1;
    WriteLog("[MP] Fcntl(fd=%d cmd=%d arg=%d) -> %d errno=%d\n", fd, cmd, arg, r, SockGetLastErrno());
    return r;
}
int SockSetSockOpt(int fd, int level, int opt, const void* val, uint32_t len) {
    int r = p_SetSockOpt ? p_SetSockOpt(fd, level, opt, val, len) : -1;
    WriteLog("[MP] SetSockOpt(fd=%d lvl=0x%x opt=0x%x) -> %d errno=%d\n", fd, level, opt, r, SockGetLastErrno());
    return r;
}
int SockBind(int fd, const void* addr, uint32_t len) {
    return p_Bind ? p_Bind(fd, addr, len) : -1;
}
int64_t SockRecvFrom(int fd, void* buf, uint64_t len, int flags, void* addr, uint32_t* addrlen) {
    return p_RecvFrom ? p_RecvFrom(fd, buf, len, flags, addr, addrlen) : -1;
}
int64_t SockSendTo(int fd, const void* buf, uint64_t len, int flags, const void* addr, uint32_t addrlen) {
    return p_SendTo ? p_SendTo(fd, buf, len, flags, addr, addrlen) : -1;
}
int SockInetPton(int af, const char* str, void* dst) {
    return p_InetPton ? p_InetPton(af, str, dst) : -1;
}
uint16_t SockInetHtons(uint16_t v) {
    return p_InetHtons ? p_InetHtons(v) : ((v & 0xff) << 8) | (v >> 8);
}
int SockGetLastErrno() {
    return p_GetLastErrno ? p_GetLastErrno() : 0;
}

bool NifmInitialize() {
    if (!p_NifmInit) return false;
    uint32_t res = p_NifmInit();
    WriteLog("[MP] nifm::Initialize -> 0x%x\n", res);
    return res == 0;
}
bool NifmSubmitNetworkRequest() {
    if (!p_NifmSubmit) return false;
    p_NifmSubmit();
    WriteLog("[MP] nifm::SubmitNetworkRequest called\n");
    return true;
}
bool NifmIsNetworkRequestOnHold() {
    return p_NifmOnHold ? p_NifmOnHold() : false;
}
bool NifmIsNetworkAvailable() {
    return p_NifmAvailable ? p_NifmAvailable() : false;
}
bool NifmGetCurrentPrimaryIpAddress(uint32_t* out_be_addr) {
    if (!p_NifmGetIp) return false;
    uint32_t res = p_NifmGetIp(out_be_addr);
    WriteLog("[MP] nifm::GetCurrentPrimaryIpAddress -> 0x%x addr=%08x\n", res, out_be_addr ? *out_be_addr : 0);
    return res == 0;
}

} // namespace mp
