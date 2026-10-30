#pragma once

#include <cstdint>
#include <cstddef>

/* Multiplayer support: runtime-resolved nn::socket / nn::nifm API.
 * All constants are BSD values for the Horizon socket service. */

namespace mp {

#pragma pack(push, 1)
struct MpSockAddrIn {          /* BSD sockaddr_in, 16 bytes */
    uint8_t  sin_len;
    uint8_t  sin_family;
    uint16_t sin_port;         /* big-endian */
    uint32_t sin_addr;         /* big-endian */
    char     sin_zero[8];
};
#pragma pack(pop)

struct MpPollFd {              /* BSD pollfd */
    int   fd;
    short events;
    short revents;
};

enum MpConst : int {
    MP_AF_INET        = 2,
    MP_SOCK_STREAM    = 1,
    MP_SOCK_DGRAM     = 2,
    MP_IPPROTO_TCP    = 6,
    MP_SOL_SOCKET     = 0xffff,
    MP_SO_REUSEADDR   = 0x4,
    MP_SO_BROADCAST   = 0x20,
    MP_TCP_NODELAY    = 1,
    MP_POLLIN         = 0x01,
    MP_POLLOUT        = 0x04,
    MP_POLLERR        = 0x08,
    MP_POLLHUP        = 0x10,
    MP_F_GETFL        = 3,
    MP_F_SETFL        = 4,
    MP_O_NONBLOCK     = 0x800,
    /* Horizon uses Linux errno; BSD values accepted defensively. */
    MP_EAGAIN         = 11,
    MP_EINPROGRESS    = 115,
    MP_EALREADY       = 114,
    MP_EISCONN        = 106,
    MP_INADDR_ANY     = 0,
};

static constexpr uint32_t MP_INADDR_BROADCAST = 0xffffffffu;

/* Resolve every symbol we need. Logs and reports missing ones. */
bool ApiResolveAll();

/* Install guard hooks on nn::socket::Initialize (both overloads) and
 * Finalize so the game cannot re-init or tear down sockets under us. */
void InstallSocketGuards();

/* Our own socket init path; retries are the caller's job.
 * Returns true when the bsd service is usable. */
bool EnsureSocketInit();
bool IsSocketInitialized();

/* While true, a game-side Finalize is swallowed (returns success). */
void SetMpEnabled(bool enabled);

/* --- socket wrappers (safe to call after ApiResolveAll) --- */
int      SockSocket(int family, int type, int proto);
int      SockConnect(int fd, const void* addr, uint32_t len);
int64_t  SockSend(int fd, const void* buf, uint64_t len, int flags);
int64_t  SockRecv(int fd, void* buf, uint64_t len, int flags);
int      SockClose(int fd);
int      SockPoll(MpPollFd* fds, uint64_t nfds, int timeout_ms);
int      SockFcntl(int fd, int cmd, int arg);
int      SockSetSockOpt(int fd, int level, int opt, const void* val, uint32_t len);
int      SockBind(int fd, const void* addr, uint32_t len);
int64_t  SockRecvFrom(int fd, void* buf, uint64_t len, int flags, void* addr, uint32_t* addrlen);
int64_t  SockSendTo(int fd, const void* buf, uint64_t len, int flags, const void* addr, uint32_t addrlen);
int      SockInetPton(int af, const char* str, void* dst);
uint16_t SockInetHtons(uint16_t v);
int      SockGetLastErrno();

/* --- nifm wrappers --- */
bool NifmInitialize();
bool NifmSubmitNetworkRequest();
bool NifmIsNetworkRequestOnHold();
bool NifmIsNetworkAvailable();
bool NifmGetCurrentPrimaryIpAddress(uint32_t* out_be_addr);

} // namespace mp
