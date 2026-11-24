#include "MpSock.hpp"
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <atomic>
#include <mutex>
#include <vector>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace mp {

static std::atomic<int> s_lastErr{0};
static std::atomic<bool> s_inited{false};
static std::atomic<bool> s_enabled{true};

/* Translate WSAGetLastError into the Linux-style values the mp code uses. */
static int XlatErr(int wsa) {
    switch (wsa) {
    case WSAEWOULDBLOCK:     return MP_EAGAIN;
    case WSAEINPROGRESS:     return MP_EINPROGRESS;
    case WSAEALREADY:        return MP_EALREADY;
    case WSAEISCONN:         return MP_EISCONN;
    case WSAECONNREFUSED:    return MP_ECONNREFUSED;
    case WSAECONNRESET:
    case WSAECONNABORTED:    return MP_ECONNRESET;
    default:                 return wsa ? wsa : 1;
    }
}

static sockaddr_in ToNative(const void* addr) {
    sockaddr_in sa{};
    const MpSockAddrIn* m = (const MpSockAddrIn*)addr;
    sa.sin_family = AF_INET;
    sa.sin_port = m->sin_port;
    sa.sin_addr.s_addr = m->sin_addr;
    return sa;
}
static MpSockAddrIn FromNative(const sockaddr_in& sa) {
    MpSockAddrIn m{};
    m.sin_family = MP_AF_INET;
    m.sin_port = sa.sin_port;
    m.sin_addr = sa.sin_addr.s_addr;
    return m;
}

bool ApiResolveAll() {
    WSADATA wd{};
    int rc = WSAStartup(MAKEWORD(2, 2), &wd);
    s_inited = (rc == 0);
    return rc == 0;
}
bool EnsureSocketInit() { return s_inited || ApiResolveAll(); }
bool IsSocketInitialized() { return s_inited; }
void SetMpEnabled(bool enabled) { s_enabled = enabled; }

int SockSocket(int family, int type, int proto) {
    SOCKET s = socket(family, type, proto);
    if (s == INVALID_SOCKET) { s_lastErr = XlatErr(WSAGetLastError()); return -1; }
    return (int)s;
}
int SockConnect(int fd, const void* addr, uint32_t len) {
    (void)len;
    sockaddr_in sa = ToNative(addr);
    int rc = connect((SOCKET)fd, (sockaddr*)&sa, sizeof(sa));
    if (rc != 0) {
        int e = WSAGetLastError();
        /* Windows reports a non-blocking connect as WSAEWOULDBLOCK;
         * the mp code (Linux semantics) expects EINPROGRESS. */
        if (e == WSAEWOULDBLOCK) e = WSAEINPROGRESS;
        s_lastErr = XlatErr(e);
        return -1;
    }
    return 0;
}
int64_t SockSend(int fd, const void* buf, uint64_t len, int flags) {
    int rc = send((SOCKET)fd, (const char*)buf, (int)len, flags);
    if (rc < 0) { s_lastErr = XlatErr(WSAGetLastError()); return -1; }
    return rc;
}
int64_t SockRecv(int fd, void* buf, uint64_t len, int flags) {
    int rc = recv((SOCKET)fd, (char*)buf, (int)len, flags);
    if (rc < 0) { s_lastErr = XlatErr(WSAGetLastError()); return -1; }
    return rc;
}
int SockClose(int fd) {
    return closesocket((SOCKET)fd) == 0 ? 0 : -1;
}
int SockPoll(MpPollFd* fds, uint64_t nfds, int timeout_ms) {
    WSAPOLLFD pf[16];
    if (nfds > 16) nfds = 16;
    for (uint64_t i = 0; i < nfds; i++) {
        pf[i].fd = (SOCKET)fds[i].fd;
        pf[i].events = 0;
        if (fds[i].events & MP_POLLIN)  pf[i].events |= POLLIN;
        if (fds[i].events & MP_POLLOUT) pf[i].events |= POLLOUT;
        pf[i].revents = 0;
    }
    int rc = WSAPoll(pf, (ULONG)nfds, timeout_ms);
    for (uint64_t i = 0; i < nfds; i++) {
        short re = pf[i].revents;
        fds[i].revents = 0;
        if (re & (POLLIN | POLLRDNORM))    fds[i].revents |= MP_POLLIN;
        if (re & (POLLOUT | POLLWRNORM))   fds[i].revents |= MP_POLLOUT;
        if (re & (POLLERR | POLLHUP | POLLNVAL)) fds[i].revents |= MP_POLLERR;
        if (re & POLLHUP)                  fds[i].revents |= MP_POLLHUP;
    }
    return rc;
}
static bool s_nonblock[1024];
int SockFcntl(int fd, int cmd, int arg) {
    if (cmd == MP_F_GETFL) {
        return (fd >= 0 && fd < 1024 && s_nonblock[fd]) ? MP_O_NONBLOCK : 0;
    }
    if (cmd == MP_F_SETFL) {
        u_long nb = (arg & MP_O_NONBLOCK) ? 1 : 0;
        if (ioctlsocket((SOCKET)fd, FIONBIO, &nb) != 0) {
            s_lastErr = XlatErr(WSAGetLastError());
            return -1;
        }
        if (fd >= 0 && fd < 1024) s_nonblock[fd] = nb != 0;
        return 0;
    }
    return 0;
}
int SockSetSockOpt(int fd, int level, int opt, const void* val, uint32_t len) {
    int l = SOL_SOCKET, o = opt;
    if (level == MP_SOL_SOCKET) {
        l = SOL_SOCKET;
        if (opt == MP_SO_BROADCAST) o = SO_BROADCAST;
        else if (opt == MP_SO_REUSEADDR) o = SO_REUSEADDR;
    } else if (level == MP_IPPROTO_TCP) {
        l = IPPROTO_TCP;
        if (opt == MP_TCP_NODELAY) o = TCP_NODELAY;
    }
    int rc = setsockopt((SOCKET)fd, l, o, (const char*)val, (int)len);
    if (rc != 0) { s_lastErr = XlatErr(WSAGetLastError()); return -1; }
    return 0;
}
int SockBind(int fd, const void* addr, uint32_t len) {
    (void)len;
    sockaddr_in sa = ToNative(addr);
    int rc = bind((SOCKET)fd, (sockaddr*)&sa, sizeof(sa));
    if (rc != 0) { s_lastErr = XlatErr(WSAGetLastError()); return -1; }
    return 0;
}
int64_t SockRecvFrom(int fd, void* buf, uint64_t len, int flags, void* addr, uint32_t* addrlen) {
    sockaddr_in sa{};
    int sl = sizeof(sa);
    int rc;
    /* UDP: Winsock surfaces ICMP port-unreachable (from an earlier sendto
     * to a dead host) as WSAECONNRESET on recv. Drain those so a subnet
     * sweep doesn't starve real replies. */
    do {
        rc = recvfrom((SOCKET)fd, (char*)buf, (int)len, flags, (sockaddr*)&sa, &sl);
    } while (rc < 0 && WSAGetLastError() == WSAECONNRESET);
    if (rc < 0) { s_lastErr = XlatErr(WSAGetLastError()); return -1; }
    if (addr) *(MpSockAddrIn*)addr = FromNative(sa);
    if (addrlen) *addrlen = 16;
    return rc;
}
int64_t SockSendTo(int fd, const void* buf, uint64_t len, int flags, const void* addr, uint32_t addrlen) {
    (void)addrlen;
    sockaddr_in sa = ToNative(addr);
    int rc = sendto((SOCKET)fd, (const char*)buf, (int)len, flags, (sockaddr*)&sa, sizeof(sa));
    if (rc < 0) { s_lastErr = XlatErr(WSAGetLastError()); return -1; }
    return rc;
}
int SockInetPton(int af, const char* str, void* dst) {
    if (!str || !dst) return 0;
    struct in_addr a;
    if (InetPtonA(af, str, &a) != 1) return 0;
    *(uint32_t*)dst = a.s_addr;
    return 1;
}
uint16_t SockInetHtons(uint16_t v) { return htons(v); }
int SockGetLastErrno() { return s_lastErr; }

bool SockResolveIpv4(const char* host, uint32_t* out_be_addr) {
    if (!host || !out_be_addr) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    *out_be_addr = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

bool NifmIsNetworkAvailable() {
    uint32_t ip = 0;
    return NifmGetCurrentPrimaryIpAddress(&ip) && ip != 0;
}
bool NifmGetCurrentPrimaryIpAddress(uint32_t* out_be_addr) {
    if (!out_be_addr) return false;
    /* Primary interface via connect-to-8.8.8.8 trick (no traffic sent). */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = htonl(0x08080808);
    connect(s, (sockaddr*)&sa, sizeof(sa));
    sockaddr_in me{};
    int sl = sizeof(me);
    bool ok = getsockname(s, (sockaddr*)&me, &sl) == 0;
    closesocket(s);
    if (!ok) return false;
    *out_be_addr = me.sin_addr.s_addr;
    return true;
}
bool NifmGetCurrentIpConfig(uint32_t* ip_be, uint32_t* mask_be) {
    ULONG sz = 0;
    GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &sz);
    std::vector<uint8_t> buf(sz);
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, 0, nullptr, aa, &sz) != NO_ERROR) return false;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_in* sin = (sockaddr_in*)u->Address.lpSockaddr;
            ULONG mask = 0;
            if (u->OnLinkPrefixLength <= 32)
                mask = (u->OnLinkPrefixLength == 0) ? 0 : (0xFFFFFFFFu << (32 - u->OnLinkPrefixLength));
            if (ip_be) *ip_be = sin->sin_addr.s_addr;
            if (mask_be) *mask_be = htonl(mask);
            return true;
        }
    }
    return false;
}
int NifmGetBroadcastAddrs(uint32_t* out, int cap) {
    int n = 0;
    if (!out || cap <= 0) return 0;
    ULONG sz = 0;
    GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &sz);
    std::vector<uint8_t> buf(sz);
    IP_ADAPTER_ADDRESSES* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, 0, nullptr, aa, &sz) != NO_ERROR) return 0;
    for (auto* a = aa; a && n < cap; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u && n < cap; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_in* sin = (sockaddr_in*)u->Address.lpSockaddr;
            if (u->OnLinkPrefixLength > 32 || u->OnLinkPrefixLength == 0) continue;
            uint32_t mask = 0xFFFFFFFFu << (32 - u->OnLinkPrefixLength);
            uint32_t ip = ntohl(sin->sin_addr.s_addr);
            uint32_t bcast = (ip & mask) | (~mask);
            out[n++] = htonl(bcast);
        }
    }
    return n;
}

} // namespace mp
