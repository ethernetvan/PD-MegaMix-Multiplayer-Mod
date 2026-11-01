#include "MpNet.hpp"
#include "MpApi.hpp"
#include "MpJson.hpp"
#include "MpInput.hpp"
#include "MpLobby.hpp"
#include "../logger.hpp"
#include "../Config.hpp"
#include "../nc/diva_nc.hpp"
#include "../overlays/SongQuickJump.hpp"
#include "lib.hpp"
#include <nn.hpp>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace ImGui { void DbgSetCursor(float x, float y, bool click); }

namespace mp {

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

static std::atomic<MpState> g_state{MpState::Disabled};
static std::mutex   g_statusMtx;
static char         g_server[64]   = "";
static int          g_rtt          = -1;
static char         g_lastError[96] = "";
static std::atomic<uint32_t> g_connectCount{0};

const char* StateName(MpState s) {
    switch (s) {
        case MpState::Disabled:       return "off";
        case MpState::WaitingNetwork: return "net-wait";
        case MpState::SocketInit:     return "sock-init";
        case MpState::Discovering:    return "discover";
        case MpState::Connecting:     return "connect";
        case MpState::Connected:      return "online";
        case MpState::Error:          return "error";
    }
    return "?";
}

MpState GetState() { return g_state.load(std::memory_order_acquire); }

void GetStatus(MpStatus* out) {
    out->state = GetState();
    out->connect_count = g_connectCount.load();
    std::lock_guard<std::mutex> lk(g_statusMtx);
    size_t ns_ = strnlen(g_server, sizeof(g_server));
    size_t ne_ = strnlen(g_lastError, sizeof(g_lastError));
    if (ns_ >= sizeof(out->server)) ns_ = sizeof(out->server) - 1;
    if (ne_ >= sizeof(out->last_error)) ne_ = sizeof(out->last_error) - 1;
    memcpy(out->server, g_server, ns_);
    out->server[ns_] = 0;
    out->rtt_ms = g_rtt;
    memcpy(out->last_error, g_lastError, ne_);
    out->last_error[ne_] = 0;
}

static void SetState(MpState s) {
    MpState prev = g_state.exchange(s, std::memory_order_release);
    if (prev != s)
        WriteLog("[MP] state %s -> %s\n", StateName(prev), StateName(s));
}

static void SetError(const char* fmt, ...) {
    char buf[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    WriteLog("[MP] error: %s\n", buf);
    std::lock_guard<std::mutex> lk(g_statusMtx);
    size_t n_ = strnlen(buf, sizeof(g_lastError) - 1);
    memcpy(g_lastError, buf, n_);
    g_lastError[n_] = 0;
}

static void SetServer(const char* s) {
    std::lock_guard<std::mutex> lk(g_statusMtx);
    size_t n_ = strnlen(s, sizeof(g_server) - 1);
    memcpy(g_server, s, n_);
    g_server[n_] = 0;
}

static void SetRtt(int ms) {
    std::lock_guard<std::mutex> lk(g_statusMtx);
    g_rtt = ms;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

static uint64_t NowMs() {
    uint64_t tick = nn::os::GetSystemTick().GetInt64Value();
    uint64_t freq = nn::os::GetSystemTickFrequency();
    return (tick * 1000) / freq;
}

static void SleepMs(uint32_t ms) {
    nn::os::SleepThread(nn::TimeSpan::FromMilliSeconds(ms));
}

/* ------------------------------------------------------------------ */
/* Outgoing queue: fixed ring 128 x 512B, memcpy-only under lock        */
/* ------------------------------------------------------------------ */

struct OutSlot { uint16_t len; char data[510]; };
static OutSlot      s_outQ[128];
static uint32_t     s_outHead = 0;   /* oldest */
static uint32_t     s_outCount = 0;
static std::mutex   s_outMtx;

void Enqueue(const char* line) {
    if (line == nullptr) return;
    size_t n = strlen(line);
    if (n == 0) return;
    if (n > sizeof(((OutSlot*)0)->data) - 1)
        n = sizeof(((OutSlot*)0)->data) - 1;

    std::lock_guard<std::mutex> lk(s_outMtx);
    uint32_t idx;
    if (s_outCount == 128) {
        /* full: overwrite oldest */
        idx = s_outHead;
        s_outHead = (s_outHead + 1) & 127;
        s_outCount--;
    }
    idx = (s_outHead + s_outCount) & 127;
    memcpy(s_outQ[idx].data, line, n);
    s_outQ[idx].data[n] = '\n';
    s_outQ[idx].len = (uint16_t)(n + 1);
    s_outCount++;
}

/* Pop at most max bytes into buf. Returns bytes written. */
static size_t DrainQueue(char* buf, size_t max) {
    std::lock_guard<std::mutex> lk(s_outMtx);
    size_t w = 0;
    while (s_outCount > 0 && w + s_outQ[s_outHead].len <= max) {
        memcpy(buf + w, s_outQ[s_outHead].data, s_outQ[s_outHead].len);
        w += s_outQ[s_outHead].len;
        s_outHead = (s_outHead + 1) & 127;
        s_outCount--;
    }
    return w;
}

/* ------------------------------------------------------------------ */
/* Wire helpers                                                        */
/* ------------------------------------------------------------------ */

static int g_sock = -1;
static char s_lineBuf[16 * 1024];
static uint32_t s_lineLen = 0;

/* Horizon sockets use Linux errnos; BSD values accepted defensively. */
static bool IsEagain() {
    int e = SockGetLastErrno();
    return e == MP_EAGAIN || e == 35;
}
static bool IsInProgress() {
    int e = SockGetLastErrno();
    return e == MP_EINPROGRESS || e == MP_EALREADY || e == 36 || e == 37;
}
static bool IsConnected() {
    int e = SockGetLastErrno();
    return e == MP_EISCONN || e == 56;
}

static bool SendAll(const char* data, size_t len) {
    size_t off = 0;
    uint64_t deadline = NowMs() + 1000;
    while (off < len) {
        int64_t r = SockSend(g_sock, data + off, len - off, 0);
        if (r > 0) { off += (size_t)r; continue; }
        if (r < 0 && IsEagain()) {
            if (NowMs() >= deadline) {
                WriteLog("[MP] send deadline exceeded errno=%d\n",
                         SockGetLastErrno());
                return false;
            }
            SleepMs(5);
            continue;
        }
        WriteLog("[MP] send failed r=%lld errno=%d\n", r, SockGetLastErrno());
        return false;
    }
    return true;
}

static void FlushOutgoing() {
    char buf[16 * 1024];
    size_t n = DrainQueue(buf, sizeof(buf));
    if (n > 0 && !SendAll(buf, n))
        WriteLog("[MP] flush failed (%d bytes dropped)\n", (int)n);
}

/* Process one complete line (no trailing \n). */
static uint32_t s_pingSeq = 0;
static uint64_t s_lastPingSent = 0;
static uint64_t s_lastRecv = 0;

static void PushDbgCmd(const char* line, jsmntok_t* tok, int ntok);

static void HandleLine(const char* line, int len) {
    if (len <= 0) return;
    jsmntok_t tok[48];
    int ntok = mpjson::Parse(line, len, tok, 48);
    if (ntok == JSMN_ERROR_NOMEM) {
        /* big lobby messages (member lists) exceed the small token budget we
         * need here only to read "t"; hand them to the lobby parser as-is */
        lobby::PushLine(line, len);
        return;
    }
    if (ntok <= 0) {
        WriteLog("[MP] bad json: %.80s\n", line);
        return;
    }
    int tIdx = mpjson::Find(line, tok, ntok, "t");
    char type[32] = "";
    if (tIdx >= 0) mpjson::Copy(line, tok[tIdx], type, sizeof(type));

    if (strcmp(type, "pong") == 0) {
        int seq = -1;
        int sIdx = mpjson::Find(line, tok, ntok, "seq");
        if (sIdx >= 0) mpjson::ToInt(line, tok[sIdx], &seq);
        if (seq >= 0)
            SetRtt((int)(NowMs() - s_lastPingSent));
        return;
    }
    if (strcmp(type, "dbg_cmd") == 0) {
        PushDbgCmd(line, tok, ntok);
        return;
    }
    lobby::PushLine(line, len);            /* lobby protocol -> game thread */
}

/* ------------------------------------------------------------------ */
/* Debug command channel (net thread -> game thread via POD queue)     */
/* ------------------------------------------------------------------ */

enum class MpCmdType : uint8_t {
    None, Tap, Spam, SpamStop, BlockSet, BlockClear, Jump, Autopause, ListPvs,
    LobbyCreate, LobbyJoin, LobbyLeave, SetName, Overlay, Cursor
};
struct MpCmd {
    MpCmdType type;
    uint64_t  buttons;
    int32_t   a, b;      /* hold/gap, hz/secs, pv */
    int32_t   count;
    char      str[40];
};

static MpCmd     s_cmdQ[16];
static uint32_t  s_cmdHead = 0, s_cmdCount = 0;
static std::mutex s_cmdMtx;

static void PushCmd(const MpCmd& c) {
    std::lock_guard<std::mutex> lk(s_cmdMtx);
    if (s_cmdCount == 16) { s_cmdHead = (s_cmdHead + 1) & 15; s_cmdCount--; }
    s_cmdQ[(s_cmdHead + s_cmdCount) & 15] = c;
    s_cmdCount++;
}

static bool PopCmd(MpCmd* c) {
    std::lock_guard<std::mutex> lk(s_cmdMtx);
    if (s_cmdCount == 0) return false;
    *c = s_cmdQ[s_cmdHead];
    s_cmdHead = (s_cmdHead + 1) & 15;
    s_cmdCount--;
    return true;
}

static void JsonBtnMask(const char* line, jsmntok_t* tok, int ntok,
                        const char* key, uint64_t* out) {
    int i = mpjson::Find(line, tok, ntok, key);
    char names[64] = "";
    if (i >= 0) mpjson::Copy(line, tok[i], names, sizeof(names));
    *out = input::MaskFromNames(names);
}

static void PushDbgCmd(const char* line, jsmntok_t* tok, int ntok) {
    int cIdx = mpjson::Find(line, tok, ntok, "cmd");
    char cmd[24] = "";
    if (cIdx >= 0) mpjson::Copy(line, tok[cIdx], cmd, sizeof(cmd));

    MpCmd c{};
    auto getInt = [&](const char* key, int32_t* out) {
        int i = mpjson::Find(line, tok, ntok, key);
        if (i < 0) return false;
        int v;
        if (!mpjson::ToInt(line, tok[i], &v)) return false;
        *out = v;
        return true;
    };

    if (strcmp(cmd, "tap") == 0) {
        c.type = MpCmdType::Tap;
        JsonBtnMask(line, tok, ntok, "btn", &c.buttons);
        c.a = 50; c.b = 50; c.count = 1;
        getInt("hold", &c.a); getInt("gap", &c.b); getInt("count", &c.count);
    } else if (strcmp(cmd, "spam") == 0) {
        int st = mpjson::Find(line, tok, ntok, "stop");
        bool stop = st >= 0 && tok[st].type == JSMN_PRIMITIVE &&
                    line[tok[st].start] == 't';
        if (stop) { c.type = MpCmdType::SpamStop; }
        else {
            c.type = MpCmdType::Spam;
            JsonBtnMask(line, tok, ntok, "btn", &c.buttons);
            c.a = 8; c.b = 0;
            getInt("hz", &c.a); getInt("secs", &c.b);
        }
    } else if (strcmp(cmd, "block") == 0) {
        c.type = MpCmdType::BlockSet;
        JsonBtnMask(line, tok, ntok, "btn", &c.buttons);
    } else if (strcmp(cmd, "unblock") == 0) {
        c.type = MpCmdType::BlockClear;
    } else if (strcmp(cmd, "jump") == 0) {
        c.type = MpCmdType::Jump;
        getInt("pv", &c.a);
    } else if (strcmp(cmd, "autopause") == 0) {
        c.type = MpCmdType::Autopause;
        int o = mpjson::Find(line, tok, ntok, "on");
        c.a = (o >= 0 && line[tok[o].start] == 't') ? 1 : 0;
    } else if (strcmp(cmd, "list_pvs") == 0) {
        c.type = MpCmdType::ListPvs;
    } else if (strcmp(cmd, "lobby_create") == 0) {
        c.type = MpCmdType::LobbyCreate;
    } else if (strcmp(cmd, "lobby_join") == 0) {
        c.type = MpCmdType::LobbyJoin;
        int i = mpjson::Find(line, tok, ntok, "code");
        if (i >= 0) mpjson::Copy(line, tok[i], c.str, sizeof(c.str));
    } else if (strcmp(cmd, "lobby_leave") == 0) {
        c.type = MpCmdType::LobbyLeave;
    } else if (strcmp(cmd, "set_name") == 0) {
        c.type = MpCmdType::SetName;
        int i = mpjson::Find(line, tok, ntok, "name");
        if (i >= 0) mpjson::Copy(line, tok[i], c.str, sizeof(c.str));
    } else if (strcmp(cmd, "overlay") == 0) {
        /* {cmd:overlay, open:bool} - open/close the MP overlay */
        c.type = MpCmdType::Overlay;
        int o = mpjson::Find(line, tok, ntok, "open");
        c.a = (o >= 0 && line[tok[o].start] == 't') ? 1 : 0;
    } else if (strcmp(cmd, "cursor") == 0) {
        /* {cmd:cursor, x, y, click:bool} - move the ImGui cursor (720p coords) */
        c.type = MpCmdType::Cursor;
        getInt("x", &c.a); getInt("y", &c.b);
        int cl = mpjson::Find(line, tok, ntok, "click");
        c.count = (cl >= 0 && line[tok[cl].start] == 't') ? 1 : 0;
    } else {
        return;
    }
    WriteLog("[MP] dbg_cmd %s\n", cmd);
    PushCmd(c);
}

/* Game-thread side: called at the top of nvnImguiCalc every frame. */
static std::atomic<int>  g_autopauseArm{0};
static std::atomic<int32_t> g_pendingJump{-1};

static void SendPvList() {
    char name[32];
    int32_t id;
    int i = 0, total = 0;
    while (SongQuickJump::GetSongDbEntry(total, &id, name, sizeof(name))) total++;
    while (i < total) {
        char line[640];
        int off = snprintf(line, sizeof(line), "{\"t\":\"dbg_pvs\",\"i\":%d,\"n\":%d,\"ids\":[", i, total);
        for (int k = 0; k < 20 && i < total; k++, i++) {
            SongQuickJump::GetSongDbEntry(i, &id, name, sizeof(name));
            off += snprintf(line + off, sizeof(line) - off, "%s%d", k ? "," : "", id);
        }
        off += snprintf(line + off, sizeof(line) - off, "],\"names\":[");
        for (int k = 0, j = i - 20 < 0 ? 0 : i - 20; j < i; k++, j++) {
            SongQuickJump::GetSongDbEntry(j, &id, name, sizeof(name));
            char esc[40];
            mpjson::EscapeInto(esc, sizeof(esc), name);
            off += snprintf(line + off, sizeof(line) - off, "%s\"%s\"", k ? "," : "", esc);
        }
        snprintf(line + off, sizeof(line) - off, "]}");
        Enqueue(line);
    }
}

void OnFrame() {
    MpCmd c;
    while (PopCmd(&c)) {
        switch (c.type) {
            case MpCmdType::Tap:
                for (int k = 0; k < c.count && k < 32; k++)
                    input::Tap(c.buttons, (uint32_t)c.a, (uint32_t)c.b);
                break;
            case MpCmdType::Spam:     input::Spam(c.buttons, (uint32_t)c.a, (uint32_t)c.b); break;
            case MpCmdType::SpamStop: input::StopSpam(); break;
            case MpCmdType::BlockSet: input::SetBlocked(c.buttons); break;
            case MpCmdType::BlockClear: input::ClearBlocked(); break;
            case MpCmdType::Jump:     g_pendingJump.store(c.a); break;
            case MpCmdType::Autopause: g_autopauseArm.store(c.a); break;
            case MpCmdType::ListPvs:  SendPvList(); break;
            case MpCmdType::LobbyCreate: lobby::CmdLobbyCreate(); break;
            case MpCmdType::LobbyJoin:   lobby::CmdLobbyJoin(c.str); break;
            case MpCmdType::LobbyLeave:  lobby::CmdLobbyLeave(); break;
            case MpCmdType::SetName:     lobby::CmdSetName(c.str); break;
            case MpCmdType::Overlay:     lobby::SetOverlayOpen(c.a != 0); break;
            case MpCmdType::Cursor:      ImGui::DbgSetCursor((float)c.a, (float)c.b, c.count != 0); break;
            default: break;
        }
    }
    lobby::Pump();
}

static void CloseSock() {
    if (g_sock >= 0) {
        SockClose(g_sock);
        g_sock = -1;
    }
    s_lineLen = 0;
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

/* Returns true and fills ip_be + port when a server is found. */
static bool Discover(uint32_t* ip_be, uint16_t* port) {
    SetState(MpState::Discovering);
    WriteLog("[MP] UDP discovery on port %d\n", Config::mpDiscoveryPort);

    int attempt = 0;
    while (true) {
        int u = SockSocket(MP_AF_INET, MP_SOCK_DGRAM, 0);
        if (u < 0) { WriteLog("[MP] discovery socket failed errno=%d\n", SockGetLastErrno()); SleepMs(2000); continue; }
        MpSockAddrIn bindAddr = {};
        bindAddr.sin_len = sizeof(bindAddr);
        bindAddr.sin_family = MP_AF_INET;
        bindAddr.sin_port = 0;
        bindAddr.sin_addr = MP_INADDR_ANY;
        SockBind(u, &bindAddr, sizeof(bindAddr));
        int one = 1;
        SockSetSockOpt(u, MP_SOL_SOCKET, MP_SO_BROADCAST, &one, sizeof(one));
        int fl = SockFcntl(u, MP_F_GETFL, 0);
        if (fl >= 0) SockFcntl(u, MP_F_SETFL, fl | MP_O_NONBLOCK);

        const char probe[] = "{\"t\":\"discover\",\"proto\":1}\n";
        MpSockAddrIn dst = {};
        dst.sin_len = sizeof(dst);
        dst.sin_family = MP_AF_INET;
        dst.sin_port = SockInetHtons((uint16_t)Config::mpDiscoveryPort);
        dst.sin_addr = MP_INADDR_BROADCAST;
        int64_t wr = SockSendTo(u, probe, sizeof(probe) - 1, 0, &dst, sizeof(dst));
        bool verbose = (attempt < 3 || attempt % 10 == 0);
        if (verbose)
            WriteLog("[MP] discovery #%d sendto -> %lld errno=%d\n",
                     attempt, wr, SockGetLastErrno());

        /* non-blocking recv loop: 10ms x 100 = 1s wait for reply */
        bool found = false;
        for (int i = 0; i < 100 && !found; i++) {
            char rbuf[1024];
            MpSockAddrIn src = {};
            uint32_t slen = sizeof(src);
            int64_t rn = SockRecvFrom(u, rbuf, sizeof(rbuf) - 1, 0, &src, &slen);
            if (rn <= 0) {
                if (i == 99 && verbose)
                    WriteLog("[MP] discovery #%d no reply, last errno=%d\n",
                             attempt, SockGetLastErrno());
                SleepMs(10);
                continue;
            }
            rbuf[rn] = 0;
            jsmntok_t tok[32];
            int ntok = mpjson::Parse(rbuf, (int)rn, tok, 32);
            int tIdx = ntok > 0 ? mpjson::Find(rbuf, tok, ntok, "t") : -1;
            char type[24] = "";
            if (tIdx >= 0) mpjson::Copy(rbuf, tok[tIdx], type, sizeof(type));
            int pIdx = ntok > 0 ? mpjson::Find(rbuf, tok, ntok, "port") : -1;
            int pnum = 0;
            if (pIdx >= 0) mpjson::ToInt(rbuf, tok[pIdx], &pnum);
            if (strcmp(type, "server") == 0 && pnum > 0) {
                *ip_be = src.sin_addr;
                *port = (uint16_t)pnum;
                WriteLog("[MP] discovered %u.%u.%u.%u:%d\n",
                         src.sin_addr & 0xff, (src.sin_addr >> 8) & 0xff,
                         (src.sin_addr >> 16) & 0xff, src.sin_addr >> 24, pnum);
                found = true;
            }
        }
        SockClose(u);
        if (found) return true;
        attempt++;
        SleepMs(2000);
    }
}

/* ------------------------------------------------------------------ */
/* TCP connect + session                                               */
/* ------------------------------------------------------------------ */

static bool TcpConnect(uint32_t ip_be, uint16_t port) {
    SetState(MpState::Connecting);
    g_sock = SockSocket(MP_AF_INET, MP_SOCK_STREAM, MP_IPPROTO_TCP);
    if (g_sock < 0) { SetError("socket() errno=%d", SockGetLastErrno()); return false; }

    MpSockAddrIn sa = {};
    sa.sin_len = sizeof(sa);
    sa.sin_family = MP_AF_INET;
    sa.sin_port = SockInetHtons(port);
    sa.sin_addr = ip_be;

    /* non-blocking connect with connect-retry loop; stays non-blocking */
    bool ok = false;
    int fl = SockFcntl(g_sock, MP_F_GETFL, 0);
    if (fl < 0 || SockFcntl(g_sock, MP_F_SETFL, fl | MP_O_NONBLOCK) < 0)
        WriteLog("[MP] Fcntl nonblock failed errno=%d\n", SockGetLastErrno());

    for (int i = 0; i < 60 && !ok; i++) {
        int r = SockConnect(g_sock, &sa, sizeof(sa));
        if (r == 0 || IsConnected()) {
            ok = true;
        } else if (IsInProgress()) {
            SleepMs(50);
        } else {
            SetError("connect errno=%d", SockGetLastErrno());
            break;
        }
        if (i == 59)
            SetError("connect timeout");
    }
    if (!ok) { CloseSock(); return false; }

    int one = 1;
    SockSetSockOpt(g_sock, MP_IPPROTO_TCP, MP_TCP_NODELAY, &one, sizeof(one));
    return true;
}

static bool SayHello() {
    char escName[64], escId[24];
    mpjson::EscapeInto(escName, sizeof(escName), Config::mpPlayerName.c_str());
    mpjson::EscapeInto(escId, sizeof(escId), Config::mpClientId.c_str());
    char line[256];
    snprintf(line, sizeof(line),
             "{\"t\":\"hello\",\"proto\":1,\"mod\":\"%s\",\"game\":\"1.0.6\",\"cid\":\"%s\",\"name\":\"%s\"}\n",
             "0.1.0-dev+" __DATE__ " " __TIME__, escId, escName);
    if (!SendAll(line, strlen(line))) { SetError("hello send failed"); return false; }

    /* poll-free: recv every 10ms up to 5s until a full line arrives */
    char rbuf[512];
    size_t rl = 0;
    bool gotLine = false;
    for (int i = 0; i < 500 && !gotLine; i++) {
        int64_t rn = SockRecv(g_sock, rbuf + rl, sizeof(rbuf) - 1 - rl, 0);
        if (rn > 0) {
            rl += (size_t)rn;
            rbuf[rl] = 0;
            if (strchr(rbuf, '\n')) gotLine = true;
        } else if (rn == 0) {
            SetError("welcome: peer closed"); return false;
        } else if (!IsEagain()) {
            SetError("welcome recv errno=%d", SockGetLastErrno());
            return false;
        }
        SleepMs(10);
    }
    if (!gotLine) { SetError("welcome timeout"); return false; }
    int64_t rn = (int64_t)rl;

    jsmntok_t tok[48];
    int ntok = mpjson::Parse(rbuf, (int)rn, tok, 48);
    int tIdx = ntok > 0 ? mpjson::Find(rbuf, tok, ntok, "t") : -1;
    char type[24] = "";
    if (tIdx >= 0) mpjson::Copy(rbuf, tok[tIdx], type, sizeof(type));

    if (strcmp(type, "welcome") == 0) {
        int wIdx = mpjson::Find(rbuf, tok, ntok, "warn");
        if (wIdx >= 0 && rbuf[tok[wIdx].start] != 'n') {
            char w[96];
            mpjson::Copy(rbuf, tok[wIdx], w, sizeof(w));
            WriteLog("[MP] server warning: %s\n", w);
        }
        /* Hand the welcome (and anything that arrived with it, e.g. the
         * lobby resync after a reconnect) to the normal line path. */
        size_t start = 0;
        for (size_t i = 0; i < rl; i++) {
            if (rbuf[i] == '\n') {
                HandleLine(rbuf + start, (int)(i - start));
                start = i + 1;
            }
        }
        for (size_t i = start; i < rl && s_lineLen < sizeof(s_lineBuf); i++)
            s_lineBuf[s_lineLen++] = rbuf[i];
        return true;
    }
    if (strcmp(type, "error") == 0) {
        int mIdx = mpjson::Find(rbuf, tok, ntok, "msg");
        char m[96] = "server error";
        if (mIdx >= 0) mpjson::Copy(rbuf, tok[mIdx], m, sizeof(m));
        SetError("%s", m);
        SleepMs(10000);            /* spec: retry in 10s on error msg */
        return false;
    }
    SetError("unexpected reply: %.60s", rbuf);
    return false;
}

static void SessionLoop() {
    SetState(MpState::Connected);
    g_connectCount++;
    s_lastRecv = NowMs();
    s_lastPingSent = 0;
    uint64_t nextPing = NowMs() + 2000;

    while (true) {
        FlushOutgoing();

        /* drain recv until EAGAIN */
        char rbuf[4096];
        while (true) {
            int64_t rn = SockRecv(g_sock, rbuf, sizeof(rbuf), 0);
            if (rn > 0) {
                s_lastRecv = NowMs();
                for (int64_t i = 0; i < rn; i++) {
                    char c = rbuf[i];
                    if (c == '\n') {
                        HandleLine(s_lineBuf, (int)s_lineLen);
                        s_lineLen = 0;
                    } else if (s_lineLen >= 4096) {
                        WriteLog("[MP] line overflow, dropping conn\n");
                        return;
                    } else {
                        s_lineBuf[s_lineLen++] = c;
                    }
                }
            } else if (rn == 0) {
                WriteLog("[MP] server closed (recv=0)\n");
                return;
            } else if (IsEagain()) {
                break;
            } else {
                WriteLog("[MP] recv errno=%d\n", SockGetLastErrno());
                return;
            }
        }

        uint64_t now = NowMs();
        if (now >= nextPing) {
            char ping[64];
            snprintf(ping, sizeof(ping), "{\"t\":\"ping\",\"seq\":%u,\"ct\":%llu}\n",
                     ++s_pingSeq, (unsigned long long)now);
            if (!SendAll(ping, strlen(ping))) { WriteLog("[MP] ping send failed\n"); return; }
            s_lastPingSent = now;
            nextPing = now + 2000;
        }
        if (now - s_lastRecv > 6000) {
            WriteLog("[MP] receive timeout\n");
            return;
        }
        SleepMs(10);
    }
}

/* ------------------------------------------------------------------ */
/* Thread                                                              */
/* ------------------------------------------------------------------ */

static void NetThread(void*) {
    SetState(MpState::WaitingNetwork);

    WriteLog("[MP] nifm init...\n");
    NifmInitialize();
    WriteLog("[MP] nifm submit...\n");
    NifmSubmitNetworkRequest();
    WriteLog("[MP] nifm wait onhold...\n");
    for (int i = 0; i < 150 && NifmIsNetworkRequestOnHold(); i++)
        SleepMs(100);
    WriteLog("[MP] nifm available=%d\n", NifmIsNetworkAvailable());
    uint32_t ip = 0;
    if (NifmIsNetworkAvailable() && NifmGetCurrentPrimaryIpAddress(&ip))
        WriteLog("[MP] primary ip %u.%u.%u.%u\n", ip & 0xff, (ip >> 8) & 0xff,
                 (ip >> 16) & 0xff, ip >> 24);

    SetState(MpState::SocketInit);
    /* prefer the game's own socket init (its net manager runs at boot);
     * wait up to 20s, then fall back to initializing ourselves. */
    for (int i = 0; i < 200 && !IsSocketInitialized(); i++)
        SleepMs(100);
    if (IsSocketInitialized()) {
        WriteLog("[MP] sockets initialized by game\n");
    } else {
        while (!EnsureSocketInit())
            SleepMs(5000);
        WriteLog("[MP] sockets initialized by mp\n");
    }

    int backoff = 0;
    const int backoffSeq[] = {1000, 2000, 4000, 5000};

    while (true) {
        uint32_t ip_be = 0;
        uint16_t port = 0;
        bool haveAddr = false;

        if (!Config::mpServerIp.empty()) {
            if (SockInetPton(MP_AF_INET, Config::mpServerIp.c_str(), &ip_be) == 1) {
                port = (uint16_t)Config::mpServerPort;
                haveAddr = true;
            } else {
                WriteLog("[MP] bad server_ip '%s'\n", Config::mpServerIp.c_str());
            }
        }
        if (!haveAddr)
            haveAddr = Discover(&ip_be, &port);

        char ab[64];
        snprintf(ab, sizeof(ab), "%u.%u.%u.%u:%u",
                 ip_be & 0xff, (ip_be >> 8) & 0xff, (ip_be >> 16) & 0xff,
                 ip_be >> 24, port);
        SetServer(ab);

        if (!TcpConnect(ip_be, port) || !SayHello()) {
            CloseSock();
            SleepMs(backoffSeq[backoff < 3 ? backoff++ : 3]);
            continue;
        }
        backoff = 0;
        SessionLoop();
        CloseSock();
        SleepMs(backoffSeq[backoff < 3 ? backoff++ : 3]);
    }
}

void Init() {
    lobby::Init();
    if (!ApiResolveAll()) {
        WriteLog("[MP] disabled: unresolved symbols\n");
        SetState(MpState::Error);
        SetError("missing symbols");
        return;
    }
    SetMpEnabled(true);

    static nn::os::ThreadType s_thread;
    /* heap stack (aligned for svc mapping safety on hardware); nn::os
     * thread priorities are 0..31 (kernel = 28+) — 31 = lowest, intended.
     * npdm caps cores 0-2. */
    void* stack = aligned_alloc(0x1000, 0x10000);
    if (!stack) {
        WriteLog("[MP] thread stack alloc failed\n");
        SetState(MpState::Error);
        return;
    }

    Result res = nn::os::CreateThread(&s_thread, NetThread, nullptr,
                                      stack, 0x10000, 31, 2);
    if (R_FAILED(res)) {
        WriteLog("[MP] CreateThread failed 0x%x\n", res);
        free(stack);
        SetState(MpState::Error);
        return;
    }
    nn::os::SetThreadName(&s_thread, "MpNet");
    nn::os::StartThread(&s_thread);
    WriteLog("[MP] net thread started\n");
}

/* ------------------------------------------------------------------ */
/* Part C: debug gameplay telemetry                                    */
/* ------------------------------------------------------------------ */

struct DbgSnap {
    bool     loaded, paused, scoring;
    uint8_t  byte2;
    int32_t  score, combo, max_combo, life;
    int32_t  judge[5];
    float    pct;
    int32_t  type, diff, pv_id;
    uint32_t ps, ip, iv, pe;   /* extra global flags */
    int32_t  res;
};

static DbgSnap  s_prev;
static bool     s_prevValid = false;
static bool     s_prevLoaded = false, s_prevPaused = false;
static uint32_t s_prevPs = 0;
static uint32_t s_frame = 0;
static uint64_t s_lastEmit = 0;

static struct {
    bool     active = false;
    uint64_t t0 = 0;
    uint64_t stLoaded = 0;
    uint64_t lastTap = 0;
} s_ap;

static void EmitState(const DbgSnap& cur, uint64_t st) {
    char line[640];
    snprintf(line, sizeof(line),
        "{\"t\":\"dbg_state\",\"f\":%u,\"loaded\":%d,\"paused\":%d,\"byte2\":%u,"
        "\"score\":%d,\"combo\":%d,\"max_combo\":%d,\"life\":%d,\"pct\":%.2f,"
        "\"jc\":[%d,%d,%d,%d,%d],\"scoring\":%d,\"type\":%d,\"diff\":%d,\"pv\":%d,"
        "\"ps\":%u,\"ip\":%u,\"iv\":%u,\"pe\":%u,\"res\":%d,\"st\":%llu}",
        s_frame, cur.loaded, cur.paused, cur.byte2, cur.score, cur.combo,
        cur.max_combo, cur.life, (double)cur.pct,
        cur.judge[0], cur.judge[1], cur.judge[2], cur.judge[3], cur.judge[4],
        cur.scoring, cur.type, cur.diff, cur.pv_id,
        cur.ps, cur.ip, cur.iv, cur.pe, cur.res,
        (unsigned long long)st);
    Enqueue(line);
}

void OnPvGameFrame() {
    PVGameData* pv = GetPVGameData();
    if (pv == nullptr) return;

    lobby::OnPvGameFrame(pv);

    uint64_t now = NowMs();

    DbgSnap cur{};
    cur.loaded   = pv->loaded;
    cur.paused   = pv->paused;
    cur.byte2    = pv->byte2;
    cur.score    = pv->score;
    cur.combo    = pv->combo;
    cur.max_combo = pv->max_combo;
    cur.life     = pv->life;
    cur.pct      = pv->percentage;
    cur.scoring  = pv->scoring_enabled;
    memcpy(cur.judge, pv->judge_count, sizeof(cur.judge));

    PvGameplayInfo* gi = GetPvGameplayInfo();
    cur.type = gi ? gi->type : -1;
    cur.diff = gi ? gi->difficulty : -1;
    auto* lp = game::GetPVLoadParam();
    cur.pv_id = lp ? lp->data[lp->data[0].int8].pv_id : -1;

    /* extra state flags read directly at main base + offset */
    uintptr_t base = exl::util::GetMainModuleInfo().m_Total.m_Start;
    cur.ps = *(volatile uint32_t*)(base + 0xBE9B1048);  /* pause state (3=paused) */
    cur.ip = *(volatile uint32_t*)(base + 0x4D285858);  /* is_play */
    cur.iv = *(volatile uint32_t*)(base + 0x4D2C9990);  /* is_pv */
    cur.pe = *(volatile uint32_t*)(base + 0xB2E1BBCC);  /* pv_end */
    cur.res = IsInSongResults() ? 1 : 0;

    uint64_t st = pv->pv_data.script_time;
    bool transition = false;
    if (cur.loaded != s_prevLoaded) {
        WriteLog("[MP] pv loaded=%d\n", cur.loaded);
        if (g_autopauseArm.load() && cur.loaded && !s_prevLoaded) {
            s_ap.active = true;
            s_ap.t0 = now;
            s_ap.stLoaded = st;
            s_ap.lastTap = 0;
            WriteLog("[MP] autopause armed: loaded st=%llu\n",
                     (unsigned long long)st);
        }
        s_prevLoaded = cur.loaded;
        transition = true;
    }
    if (cur.paused != s_prevPaused) {
        WriteLog("[MP] pv paused=%d\n", cur.paused);
        s_prevPaused = cur.paused;
        transition = true;
    }
    if (cur.ps != s_prevPs) {
        s_prevPs = cur.ps;
        transition = true;
    }

    /* autopause: tap Plus until paused or 3s window ends */
    if (s_ap.active) {
        if (cur.paused) {
            s_ap.active = false;
            char m[192];
            snprintf(m, sizeof(m),
                "{\"t\":\"dbg_autopause\",\"ok\":true,\"ms\":%llu,"
                "\"st_loaded\":%llu,\"st_paused\":%llu}",
                (unsigned long long)(now - s_ap.t0),
                (unsigned long long)s_ap.stLoaded,
                (unsigned long long)st);
            Enqueue(m);
        } else if (now - s_ap.t0 > 3000) {
            s_ap.active = false;
            char m[192];
            snprintf(m, sizeof(m),
                "{\"t\":\"dbg_autopause\",\"ok\":false,\"ms\":%llu,"
                "\"st_loaded\":%llu,\"st_paused\":%llu}",
                (unsigned long long)(now - s_ap.t0),
                (unsigned long long)s_ap.stLoaded,
                (unsigned long long)st);
            Enqueue(m);
        } else if (now - s_ap.lastTap >= 150) {
            input::Tap(nn::hid::Plus, 50, 100, true);
            s_ap.lastTap = now;
        }
    }

    if (transition || (now - s_lastEmit >= 16 &&
        (!s_prevValid || memcmp(&cur, &s_prev, sizeof(cur)) != 0))) {
        s_prev = cur;
        s_prevValid = true;
        s_frame++;
        s_lastEmit = now;
        EmitState(cur, st);
    }
}

/* ------------------------------------------------------------------ */
/* Song-select telemetry                                               */
/* ------------------------------------------------------------------ */

struct SelSnap {
    int32_t state, pv_id, diff, edition, sort_mode, index;
    uint8_t is_busy;
};
static SelSnap  s_prevSel;
static bool     s_prevSelValid = false;
static uint32_t s_selFrame = 0;

static inline int32_t  RdI32(const void* p, size_t off) { return *(int32_t*)((const char*)p + off); }
static inline uint8_t  RdU8 (const void* p, size_t off) { return *(uint8_t*)((const char*)p + off); }

static int32_t s_jumpTarget = -1;
static int     s_jumpCheck = 0;

void OnPvSelFrame(const void* sel) {
    if (sel == nullptr) return;

    lobby::OnPvSelFrame(sel);

    /* pending MP jump: execute only while selector is active & idle */
    int32_t pend = g_pendingJump.load();
    if (pend > 0 &&
        RdI32(sel, 0x68) == 6 && RdU8(sel, 0x2587C) == 0) {
        if (SongQuickJump::JumpSelectorToPv(const_cast<void*>(sel), pend, true)) {
            s_jumpTarget = pend;
            s_jumpCheck = 90;
        } else {
            char m[96];
            snprintf(m, sizeof(m),
                "{\"t\":\"dbg_jump\",\"pv\":%d,\"ok\":false,\"got\":-1}", pend);
            Enqueue(m);
        }
        g_pendingJump.store(-1);
    }
    if (s_jumpCheck > 0 && --s_jumpCheck == 0) {
        int32_t got = RdI32(sel, 0x2617C);
        int32_t d = RdI32(sel, 0x261A0);
        char m[128];
        snprintf(m, sizeof(m),
            "{\"t\":\"dbg_jump\",\"pv\":%d,\"ok\":%s,\"got\":%d,\"diff\":%d}",
            s_jumpTarget, got == s_jumpTarget ? "true" : "false", got, d);
        Enqueue(m);
        s_jumpTarget = -1;
    }

    SelSnap cur{};
    cur.state      = RdI32(sel, 0x68);
    cur.pv_id      = RdI32(sel, 0x2617C);
    cur.diff       = RdI32(sel, 0x261A0);
    cur.edition    = RdI32(sel, 0x261A4);
    cur.is_busy    = RdU8 (sel, 0x2587C);
    cur.sort_mode  = RdI32(sel, 0x26198);
    cur.index      = RdI32(sel, 0x25960);

    if (!s_prevSelValid || memcmp(&cur, &s_prevSel, sizeof(cur)) != 0) {
        s_prevSel = cur;
        s_prevSelValid = true;
        char line[256];
        snprintf(line, sizeof(line),
            "{\"t\":\"dbg_sel\",\"f\":%u,\"state\":%d,\"pv\":%d,\"diff\":%d,"
            "\"ed\":%d,\"busy\":%d,\"sort\":%d,\"idx\":%d}",
            ++s_selFrame, cur.state, cur.pv_id, cur.diff, cur.edition,
            cur.is_busy, cur.sort_mode, cur.index);
        Enqueue(line);
    }
}

void OnPvSelInit() {
    Enqueue("{\"t\":\"dbg_sel_init\"}");
    lobby::OnPvSelInit();
}
void OnPvSelDest() {
    s_prevSelValid = false;
    Enqueue("{\"t\":\"dbg_sel_dest\"}");
    lobby::OnPvSelDest();
}

/* ------------------------------------------------------------------ */
/* Results telemetry                                                   */
/* ------------------------------------------------------------------ */

void OnStageResult(bool init) {
    lobby::OnStageResult(init);
    PVGameData* pv = GetPVGameData();
    char line[384];
    if (pv) {
        snprintf(line, sizeof(line),
            "{\"t\":\"dbg_result\",\"ev\":\"%s\",\"score\":%d,\"combo\":%d,"
            "\"max_combo\":%d,\"pct\":%.2f,\"life\":%d,"
            "\"jc\":[%d,%d,%d,%d,%d]}",
            init ? "init" : "dest", pv->score, pv->combo, pv->max_combo,
            (double)pv->percentage, pv->life,
            pv->judge_count[0], pv->judge_count[1], pv->judge_count[2],
            pv->judge_count[3], pv->judge_count[4]);
    } else {
        snprintf(line, sizeof(line),
            "{\"t\":\"dbg_result\",\"ev\":\"%s\",\"score\":null}",
            init ? "init" : "dest");
    }
    Enqueue(line);
}

} // namespace mp
