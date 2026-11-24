#include "MpNet.hpp"
#include "../platform/MpSock.hpp"
#include "MpAvatar.hpp"
#include "../Stubs.hpp"            /* moduledb + AutoPlay stubs */
#include "MpJson.hpp"
#include "MpInput.hpp"
#include "MpLobby.hpp"
#include "MpMenu.hpp"
#include "MpSpr.hpp"
#include "../platform/MpLog.hpp"
#include "../Config.hpp"
#include "../game/diva.h"
#include "../game/SongQuickJump.hpp"
#include "../platform/NxShim.hpp"
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <string.h>
#include <cstdint>
#include <thread>
#include <intrin.h>
#undef CreateFile
#undef DeleteFile
#undef CreateDirectory
#undef MoveFile
#undef CopyFile
#define strncasecmp _strnicmp
#define strcasecmp  _stricmp
#define __builtin_bswap32(v) ((uint32_t)_byteswap_ulong((uint32_t)(v)))
#define __builtin_popcount(v) ((int)__popcnt((uint32_t)(v)))
using diva::PVGameData;
using diva::PvGameplayInfo;
using diva::game::GetPVGameData;
using diva::game::GetPvGameplayInfo;
using diva::game::IsInSongResults;
namespace game { using diva::game::GetPVLoadParam; }


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
/* server selection, mirrored from Config::mpServerMode / mpCloudUrl (the
 * net thread never reads those std::strings); g_modeGen bumps on every
 * change */
static std::atomic<uint8_t>  g_mode{kServerLocal};
static std::atomic<uint32_t> g_modeGen{0};
static char     s_cloudHost[128] = "mikumod.evanthomas.dev";   /* g_statusMtx */
static char     s_cloudPath[64]  = "/switch";
static uint16_t s_cloudPort      = 80;
static int      s_lastHttpCode   = 0;   /* net thread: last non-101 upgrade answer */
static uint32_t s_rateLimitMs    = 0;   /* net thread: HTTP-429 backoff ratchet */

ServerMode GetServerMode() { return (ServerMode)g_mode.load(std::memory_order_acquire); }
bool IsCloudMode() { return GetServerMode() == kServerCloud; }
const char* CloudUrl() { return Config::mpCloudUrl.c_str(); }

/* [scheme://]host[:port][/path] -> parts; port 80 and /switch by default */
static bool ParseCloudUrl(const char* url, char* host, size_t hostN, uint16_t* port,
                          char* path, size_t pathN) {
    while (*url == ' ') url++;
    for (const char* sch : {"ws://", "wss://", "http://", "https://"}) {
        size_t n = strlen(sch);
        if (strncasecmp(url, sch, n) == 0) { url += n; break; }
    }
    size_t hl = strcspn(url, ":/");
    if (hl == 0 || hl >= hostN) return false;
    for (size_t i = 0; i < hl; i++) {
        char c = url[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_'))
            return false;
        host[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    host[hl] = 0;
    const char* p = url + hl;
    unsigned pt = 80;
    if (*p == ':') {
        char* end = nullptr;
        unsigned long v = strtoul(p + 1, &end, 10);
        if (end == p + 1 || v == 0 || v > 65535) return false;
        pt = (unsigned)v;
        p = end;
    }
    *port = (uint16_t)pt;
    /* nothing, or a /path without spaces ("/" alone = the default path) */
    if (*p != 0 && (*p != '/' || strchr(p, ' '))) return false;
    size_t pl = strlen(p);
    if (pl <= 1) { snprintf(path, pathN, "/switch"); return true; }
    if (pl >= pathN) return false;
    memcpy(path, p, pl + 1);
    return true;
}

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
    out->cloud = IsCloudMode();
    out->mode = (uint8_t)GetServerMode();
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
/* normal lines stay under 4 KiB; avatar_px (one 96x96 RGBA portrait,
 * base64) is ~49 KiB */
constexpr uint32_t kMaxLine = 4096, kMaxPxLine = 52 * 1024;
static char s_lineBuf[kMaxPxLine];
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

static bool SendRaw(const char* data, size_t len) {
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

/* ------------------------------------------------------------------ */
/* WebSocket transport (online server): RFC 6455 client framing over a  */
/* plain TCP socket. The NDJSON stream is unchanged: every send becomes */
/* one masked binary message; incoming message payloads are fed to the  */
/* same line assembler as raw TCP bytes.                                */
/* ------------------------------------------------------------------ */

static bool s_ws = false;                /* current session is WebSocket */
static uint64_t s_rng = 0;

static uint32_t Rand32() {
    if (s_rng == 0)
        s_rng = (exl::util::GetRandomU64() ^
                 (uint64_t)nn::os::GetSystemTick().GetInt64Value()) | 1;
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static bool WsSendFrame(uint8_t op, const char* data, size_t len) {
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = 0x80 | op;
    if (len < 126) {
        hdr[h++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)len;
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    uint32_t m = Rand32();
    uint8_t mask[4] = {(uint8_t)m, (uint8_t)(m >> 8), (uint8_t)(m >> 16), (uint8_t)(m >> 24)};
    memcpy(hdr + h, mask, 4);
    h += 4;
    char buf[4096];
    memcpy(buf, hdr, h);
    size_t fill = h, off = 0;
    while (off < len || fill) {
        while (off < len && fill < sizeof(buf)) {
            buf[fill++] = data[off] ^ mask[off & 3];
            off++;
        }
        if (!SendRaw(buf, fill)) return false;
        fill = 0;
    }
    return true;
}

static bool SendAll(const char* data, size_t len) {
    return s_ws ? WsSendFrame(0x2, data, len) : SendRaw(data, len);
}

/* incremental decoder for server frames (unmasked; a mask is honoured) */
static struct {
    uint8_t  hdr[14];
    int      hlen, need;
    bool     inPayload, closed;
    uint8_t  op, mask[4];
    bool     masked;
    uint64_t left, pos;
    char     ctrl[125];
    int      clen;
} s_dec;

static void WsDecReset() { memset(&s_dec, 0, sizeof(s_dec)); }

static void WsFrameDone() {
    if (s_dec.op == 0x8) {
        s_dec.closed = true;
        WsSendFrame(0x8, s_dec.ctrl, s_dec.clen >= 2 ? 2 : 0);
    } else if (s_dec.op == 0x9) {
        WsSendFrame(0xA, s_dec.ctrl, (size_t)s_dec.clen);
    }
    s_dec.inPayload = false;
    s_dec.hlen = s_dec.clen = 0;
}

/* raw socket bytes -> data payload bytes appended to out (never more than
 * n). Control frames are handled here. */
static size_t WsDecode(const char* in, size_t n, char* out) {
    size_t w = 0;
    for (size_t i = 0; i < n && !s_dec.closed; i++) {
        uint8_t c = (uint8_t)in[i];
        if (!s_dec.inPayload) {
            s_dec.hdr[s_dec.hlen++] = c;
            if (s_dec.hlen == 2) {
                uint8_t l7 = s_dec.hdr[1] & 0x7f;
                s_dec.masked = (s_dec.hdr[1] & 0x80) != 0;
                s_dec.need = 2 + (l7 == 126 ? 2 : l7 == 127 ? 8 : 0) + (s_dec.masked ? 4 : 0);
            }
            if (s_dec.hlen < 2 || s_dec.hlen < s_dec.need) continue;
            uint8_t l7 = s_dec.hdr[1] & 0x7f;
            int p = 2;
            uint64_t len = l7;
            if (l7 == 126) { len = ((uint64_t)s_dec.hdr[2] << 8) | s_dec.hdr[3]; p = 4; }
            else if (l7 == 127) {
                len = 0;
                for (int k = 0; k < 8; k++) len = (len << 8) | s_dec.hdr[2 + k];
                p = 10;
            }
            if (s_dec.masked) memcpy(s_dec.mask, s_dec.hdr + p, 4);
            s_dec.op = s_dec.hdr[0] & 0x0f;
            s_dec.left = len;
            s_dec.pos = 0;
            s_dec.inPayload = true;
            if (len == 0) WsFrameDone();
            continue;
        }
        if (s_dec.masked) c ^= s_dec.mask[s_dec.pos & 3];
        s_dec.pos++;
        if (s_dec.op >= 0x8) {
            if (s_dec.clen < (int)sizeof(s_dec.ctrl)) s_dec.ctrl[s_dec.clen++] = (char)c;
        } else {
            out[w++] = (char)c;
        }
        if (--s_dec.left == 0) WsFrameDone();
    }
    return w;
}

/* recv for both transports: >0 payload bytes, 0 = peer closed (TCP FIN or
 * WebSocket close), -1 = error (errno), -2 = nothing yet */
static int64_t RecvPayload(char* buf, size_t max) {
    if (!s_ws) {
        int64_t rn = SockRecv(g_sock, buf, max, 0);
        return (rn < 0 && IsEagain()) ? -2 : rn;
    }
    if (s_dec.closed) return 0;
    char raw[4096];
    int64_t rn = SockRecv(g_sock, raw, max < sizeof(raw) ? max : sizeof(raw), 0);
    if (rn <= 0) return (rn < 0 && IsEagain()) ? -2 : rn;
    size_t w = WsDecode(raw, (size_t)rn, buf);
    if (w) return (int64_t)w;
    return s_dec.closed ? 0 : -2;
}

static void CloseSock();

/* big one-shot line (avatar_img): 64 KiB slot; '\n' appended; returns
 * false if a blob is pending / not connected / too big. */
static char         s_blob[64 * 1024 + 1];
static uint32_t     s_blobLen = 0;
static std::mutex   s_blobMtx;

bool EnqueueBlob(const char* line, size_t len) {
    std::lock_guard<std::mutex> lk(s_blobMtx);
    if (s_blobLen || g_sock < 0 || len + 1 > sizeof(s_blob)) return false;
    memcpy(s_blob, line, len);
    s_blob[len] = '\n';
    s_blobLen = (uint32_t)(len + 1);
    return true;
}

static void ClearBlob() {
    std::lock_guard<std::mutex> lk(s_blobMtx);
    s_blobLen = 0;
}

static void FlushOutgoing() {
    char buf[16 * 1024];
    size_t n = DrainQueue(buf, sizeof(buf));
    if (n > 0 && !SendAll(buf, n))
        WriteLog("[MP] flush failed (%d bytes dropped)\n", (int)n);
    /* one-shot big blob (avatar image): sent right after the ring; a
     * failed send ends the session so the stream never half-writes.
     * The producer never touches s_blob while s_blobLen != 0, so the
     * send runs outside the lock (EnqueueBlob is called on the
     * present thread - holding it across SendAll would hitch a frame) */
    uint32_t blen;
    { std::lock_guard<std::mutex> lk(s_blobMtx); blen = s_blobLen; }
    if (blen) {
        bool ok = SendAll(s_blob, blen);
        { std::lock_guard<std::mutex> lk(s_blobMtx); s_blobLen = 0; }
        if (!ok) CloseSock();
    }
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
    if (strcmp(type, "avatar_px") == 0) {
        int iIdx = mpjson::Find(line, tok, ntok, "img");
        int rIdx = mpjson::Find(line, tok, ntok, "rgba");
        char img[16] = "";
        if (iIdx >= 0) mpjson::Copy(line, tok[iIdx], img, sizeof(img));
        if (rIdx >= 0 && tok[rIdx].type == JSMN_STRING)
            avatar::OnRemotePixels(img, line + tok[rIdx].start,
                                   tok[rIdx].end - tok[rIdx].start, false);
        else
            avatar::OnRemotePixels(img, nullptr, 0, true);
        return;
    }
    if ((uint32_t)len > kMaxLine) {
        WriteLog("[MP] oversized '%s' line (%d bytes) dropped\n", type, len);
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
    LobbyCreate, LobbyJoin, LobbyLeave, SetName, Overlay, Cursor, PeekAvatar,
    PauseReq, ResumeReq,
    PokeAvatar, DumpAvatar, MenuCursor, SelLists, Autoplay, MenuHolds,
    Glow, SprTest, NotifyStyle, PopupTest, ModuleDump, RemoteAvatars, ServerMode, CloudUrl, SameDiff, NoFail, Keylog, RawTap, VkTap, WatchP, WatchOff, DevTap, TraceTap, SprTrace
};
struct MpCmd {
    MpCmdType type;
    uint64_t  buttons;
    int32_t   a, b;      /* hold/gap, hz/secs, pv */
    int32_t   count;
    char      str[104];
    int32_t   pvs[24];
    int32_t   pvsN;
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
    } else if (strcmp(cmd, "autoplay") == 0) {
        /* {cmd:autoplay, on:bool, pct?:int, mode?:name} */
        c.type = MpCmdType::Autoplay;
        int o = mpjson::Find(line, tok, ntok, "on");
        c.a = (o >= 0 && line[tok[o].start] == 't') ? 1 : 0;
        c.b = -1; getInt("pct", &c.b);
        int mi = mpjson::Find(line, tok, ntok, "mode");
        if (mi >= 0) mpjson::Copy(line, tok[mi], c.str, sizeof(c.str));
    } else if (strcmp(cmd, "notify_style") == 0) {
        /* {cmd:notify_style, style:avatars|text|off} */
        c.type = MpCmdType::NotifyStyle;
        c.a = strstr(line, "\"text\"") ? 1 : strstr(line, "\"off\"") ? 2 : 0;
    } else if (strcmp(cmd, "popup_test") == 0) {
        c.type = MpCmdType::PopupTest;
    } else if (strcmp(cmd, "module_dump") == 0) {
        c.type = MpCmdType::ModuleDump;
    } else if (strcmp(cmd, "remote_avatars") == 0) {
        /* {cmd:remote_avatars, on:bool} - same as the overlay toggle */
        c.type = MpCmdType::RemoteAvatars;
        int o = mpjson::Find(line, tok, ntok, "on");
        c.a = (o >= 0 && line[tok[o].start] == 't') ? 1 : 0;
    } else if (strcmp(cmd, "same_diff") == 0) {
        /* {cmd:same_diff, on:bool} - the ROOM CONFIG toggle (owner only) */
        c.type = MpCmdType::SameDiff;
        int o = mpjson::Find(line, tok, ntok, "on");
        c.a = (o >= 0 && line[tok[o].start] == 't') ? 1 : 0;
    } else if (strcmp(cmd, "no_fail") == 0) {
        /* {cmd:no_fail, on:bool} - the room's No Fail Mode (owner only) */
        c.type = MpCmdType::NoFail;
        int o = mpjson::Find(line, tok, ntok, "on");
        c.a = (o >= 0 && line[tok[o].start] == 't') ? 1 : 0;
    } else if (strcmp(cmd, "cloud_url") == 0) {
        /* {cmd:cloud_url, url} - same as the ONLINE pencil editor */
        c.type = MpCmdType::CloudUrl;
        int i = mpjson::Find(line, tok, ntok, "url");
        if (i >= 0) mpjson::Copy(line, tok[i], c.str, sizeof(c.str));
    } else if (strcmp(cmd, "server_mode") == 0) {
        /* {cmd:server_mode, mode:off|local|cloud} - same as the OPTIONS row */
        c.type = MpCmdType::ServerMode;
        c.a = strstr(line, "\"cloud\"") ? kServerCloud
            : strstr(line, "\"off\"") ? kServerOff : kServerLocal;
    } else if (strcmp(cmd, "glow") == 0) {
        /* {cmd:glow, dir:up|down} - debug: fire the rank-change glow */
        c.type = MpCmdType::Glow;
        c.a = (strstr(line, "\"down\"") != nullptr) ? 0 : 1;
    } else if (strcmp(cmd, "sprtest") == 0) {
        /* {cmd:sprtest, prio:N} - debug: game-sprite test rect on the clear
         * gauge at sprite priority N (-1 off) */
        c.type = MpCmdType::SprTest;
        c.a = -1; getInt("prio", &c.a);
    } else if (strcmp(cmd, "menu_holds") == 0) {
        /* {cmd:menu_holds, init, score_a, vp, menu, nav} - results page
         * hold times (ms); -1 keeps the current value */
        c.type = MpCmdType::MenuHolds;
        static const char* k[5] = {"init", "score_a", "vp", "menu", "nav"};
        c.pvsN = 5;
        for (int i = 0; i < 5; i++) {
            c.pvs[i] = -1;
            getInt(k[i], &c.pvs[i]);
        }
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
    } else if (strcmp(cmd, "pause_req") == 0) {
        c.type = MpCmdType::PauseReq;
    } else if (strcmp(cmd, "resume_req") == 0) {
        c.type = MpCmdType::ResumeReq;
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
    } else if (strcmp(cmd, "peek_avatar") == 0) {
        c.type = MpCmdType::PeekAvatar;
    } else if (strcmp(cmd, "poke_avatar") == 0) {
        /* {module} -> overwrite Shared Set A slot 1 (tests module changes) */
        c.type = MpCmdType::PokeAvatar;
        c.a = -1; getInt("module", &c.a);
    } else if (strcmp(cmd, "dump_avatar") == 0) {
        c.type = MpCmdType::DumpAvatar;
        c.a = -1; getInt("module", &c.a);
    } else if (strcmp(cmd, "menu_cursor") == 0) {
        /* {cmd:menu_cursor, v:N} - force the menu model's cursor; the
         * real-cursor resync is suppressed ~2 s so the model can be
         * desynced on purpose (retry-slip recovery test) */
        c.type = MpCmdType::MenuCursor;
        c.a = 0; getInt("v", &c.a);
    } else if (strcmp(cmd, "sel_lists") == 0) {
        /* {cmd:sel_lists, pvs:[...]} - dump the 8 sorted_pv_lists of the
         * current sort mode and which requested pvs are in each */
        c.type = MpCmdType::SelLists;
        c.pvsN = 0;
        int ai = mpjson::Find(line, tok, ntok, "pvs");
        if (ai >= 0 && tok[ai].type == JSMN_ARRAY) {
            int i = ai + 1;
            while (i < ntok && c.pvsN < 24 && tok[i].type == JSMN_PRIMITIVE) {
                int v;
                if (mpjson::ToInt(line, tok[i], &v)) c.pvs[c.pvsN++] = v;
                i++;
            }
        }
    }
#ifdef MP_DEV_TOOLS
    else if (strcmp(cmd, "keylog") == 0) {
        c.type = MpCmdType::Keylog;
        c.a = 0; getInt("on", &c.a);
    } else if (strcmp(cmd, "rawtap") == 0) {
        c.type = MpCmdType::RawTap;
        c.a = -1; c.b = 80;
        getInt("idx", &c.a); getInt("hold", &c.b);
    } else if (strcmp(cmd, "vktap") == 0) {
        c.type = MpCmdType::VkTap;
        c.a = -1; c.b = 80;
        getInt("vk", &c.a); getInt("idx", &c.a); getInt("hold", &c.b);
    } else if (strcmp(cmd, "tracetap") == 0) {
        c.type = MpCmdType::TraceTap;
        c.a = 0; getInt("on", &c.a);
    } else if (strcmp(cmd, "devtap") == 0) {
        c.type = MpCmdType::DevTap;
        c.a = -1; c.b = 80;
        getInt("idx", &c.a); getInt("hold", &c.b);
    } else if (strcmp(cmd, "watchp") == 0) {
        c.type = MpCmdType::WatchP;
    } else if (strcmp(cmd, "watchoff") == 0) {
        c.type = MpCmdType::WatchOff;
    } else if (strcmp(cmd, "sprtrace") == 0) {
        /* {cmd:sprtrace, n:N} - log the next N put_sprite submissions */
        c.type = MpCmdType::SprTrace;
        c.a = 1500; getInt("n", &c.a);
    }
#endif
    else {
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
    menu::Frame();
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
            case MpCmdType::Autoplay: {
                if (c.str[0]) {
                    int32_t md = AutoPlay::ModeFromName(c.str);
                    if (md >= 0) AutoPlay::g_mode = md;
                }
                if (c.b >= 0) {
                    AutoPlay::g_hitPct = c.b > 100 ? 100 : (c.b < 0 ? 0 : c.b);
                    AutoPlay::g_mode = AutoPlay::Mode_Custom;
                }
                AutoPlay::g_enabled = c.a != 0;
                lobby::ReportAutoPlay(AutoPlay::g_enabled);
                break;
            }
            case MpCmdType::ListPvs:  SendPvList(); break;
            case MpCmdType::LobbyCreate: lobby::CmdLobbyCreate(); break;
            case MpCmdType::LobbyJoin:   lobby::CmdLobbyJoin(c.str); break;
            case MpCmdType::LobbyLeave:  lobby::CmdLobbyLeave(); break;
            case MpCmdType::PauseReq:    lobby::CmdPauseReq(); break;
            case MpCmdType::ResumeReq:   lobby::CmdResumeReq(); break;
            case MpCmdType::SetName:     lobby::CmdSetName(c.str); break;
            case MpCmdType::Overlay:     lobby::SetOverlayOpen(c.a != 0); break;
            case MpCmdType::Cursor:      ImGui::DbgSetCursor((float)c.a, (float)c.b, c.count != 0); break;
            case MpCmdType::PeekAvatar:  avatar::DbgPeek(); break;
            case MpCmdType::PokeAvatar:  avatar::DbgPoke(c.a); break;
            case MpCmdType::DumpAvatar:  avatar::DbgDump(c.a); break;
            case MpCmdType::MenuCursor:  menu::ForceCursor(c.a); break;
            case MpCmdType::SelLists:   lobby::DbgSelLists(c.pvs, c.pvsN); break;
#ifdef MP_DEV_TOOLS
            case MpCmdType::Keylog:     input::SetKeylog(c.a); break;
            case MpCmdType::RawTap:     input::RawTap(c.a, c.b); break;
            case MpCmdType::VkTap:      input::VkTap(c.a, c.b); break;
            case MpCmdType::WatchP:
                input::WatchAddr((uintptr_t)diva::game::GetPVGameData() + 1);
                break;
            case MpCmdType::WatchOff:   input::WatchOff(); break;
            case MpCmdType::DevTap:     input::DevTap(c.a, c.b); break;
            case MpCmdType::TraceTap:   input::SetTraceTap(c.a); break;
            case MpCmdType::SprTrace:   spr::TraceArm(c.a); break;
#endif
            case MpCmdType::MenuHolds:
                menu::SetResHolds(c.pvs[0], c.pvs[1], c.pvs[2],
                                  c.pvs[3], c.pvs[4]);
                break;
            case MpCmdType::SprTest: input::g_sprTestPrio = c.a; break;
            case MpCmdType::Glow:
                mp::lobby::TriggerRankGlow(c.a != 0);
                break;
            case MpCmdType::NotifyStyle:
                mp::lobby::SetNotifyStyle(c.a);
                break;
            case MpCmdType::PopupTest:
                mp::lobby::PopupTest();
                break;
            case MpCmdType::ModuleDump:  moduledb::DbgDump(); break;
            case MpCmdType::RemoteAvatars: lobby::SetRemoteAvatars(c.a != 0); break;
            case MpCmdType::SameDiff: lobby::CmdSameDiff(c.a != 0); break;
            case MpCmdType::NoFail:   lobby::CmdNoFail(c.a != 0); break;
            case MpCmdType::ServerMode:  SetServerMode((ServerMode)c.a); break;
            case MpCmdType::CloudUrl:    SetCloudUrl(c.str); break;
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
    s_ws = false;
    WsDecReset();
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

/* Servers this console reached before, most recent first, kept on the SD
 * card across boots (net thread only). */
static const char kServersPath[] = "ExlSD:/MikuMikuSwitchPlugin/mp_servers.txt";
static const int kMaxKnown = 4;
static uint32_t s_knownIp[kMaxKnown];   /* network order */
static uint16_t s_knownPort[kMaxKnown];
static int s_nKnown = 0;
static int s_sweepTurn = 0;

static bool UsableIp(uint32_t h) {      /* host order */
    return h != 0 && (h >> 24) != 127 && (h >> 28) < 14;
}

static void LoadKnownServers() {
    nn::fs::FileHandle f;
    if (R_FAILED(nn::fs::OpenFile(&f, kServersPath, nn::fs::OpenMode_Read))) return;
    char buf[256];
    int64_t size = 0;
    nn::fs::GetFileSize(&size, f);
    if (size < 0) size = 0;
    if (size > (int64_t)sizeof(buf) - 1) size = sizeof(buf) - 1;
    if (R_FAILED(nn::fs::ReadFile(f, 0, buf, (uint64_t)size))) size = 0;
    nn::fs::CloseFile(f);
    buf[size] = 0;
    for (char* line = buf; *line && s_nKnown < kMaxKnown;) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        unsigned a, b, c, d, p;
        if (sscanf(line, "%u.%u.%u.%u %u", &a, &b, &c, &d, &p) == 5 &&
            a < 256 && b < 256 && c < 256 && d < 256 && p > 0 && p < 65536) {
            const uint32_t h = (a << 24) | (b << 16) | (c << 8) | d;
            if (UsableIp(h)) {
                s_knownIp[s_nKnown] = __builtin_bswap32(h);
                s_knownPort[s_nKnown++] = (uint16_t)p;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    WriteLog("[MP] %d remembered server(s)\n", s_nKnown);
}

static void RememberServer(uint32_t ip_be, uint16_t port) {
    const uint32_t h = __builtin_bswap32(ip_be);
    if (!UsableIp(h) || (s_nKnown && s_knownIp[0] == ip_be && s_knownPort[0] == port))
        return;
    int at = -1;
    for (int i = 0; i < s_nKnown && at < 0; i++)
        if (s_knownIp[i] == ip_be && s_knownPort[i] == port) at = i;
    if (at < 0) at = s_nKnown < kMaxKnown ? s_nKnown++ : kMaxKnown - 1;
    for (int i = at; i > 0; i--) {
        s_knownIp[i] = s_knownIp[i - 1];
        s_knownPort[i] = s_knownPort[i - 1];
    }
    s_knownIp[0] = ip_be;
    s_knownPort[0] = port;

    char out[160];
    int o = 0;
    for (int i = 0; i < s_nKnown; i++) {
        const uint32_t k = __builtin_bswap32(s_knownIp[i]);
        o += snprintf(out + o, sizeof(out) - o, "%u.%u.%u.%u %u\n", k >> 24,
                      (k >> 16) & 0xff, (k >> 8) & 0xff, k & 0xff, s_knownPort[i]);
    }
    nn::fs::DeleteFile(kServersPath);
    nn::fs::FileHandle f;
    if (R_SUCCEEDED(nn::fs::CreateFile(kServersPath, o)) &&
        R_SUCCEEDED(nn::fs::OpenFile(&f, kServersPath, nn::fs::OpenMode_Write))) {
        nn::fs::WriteFile(f, 0, out, o, nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
        nn::fs::CloseFile(f);
    }
    WriteLog("[MP] remembered server %u.%u.%u.%u:%u\n", h >> 24, (h >> 16) & 0xff,
             (h >> 8) & 0xff, h & 0xff, port);
}

/* One discovery round (~1.5 s); true fills ip_be + port. Hotspots and
 * some APs drop broadcasts, so besides 255.255.255.255 and the subnet
 * broadcast the probe also goes to 127.0.0.1 (Ryujinx: a server on the
 * same PC), the configured server_ip and every host of the local subnet
 * (the whole subnet up to a /22, otherwise our /24). Networks split into
 * routed subnets (PC and console on different ones) never pass that, so
 * the probe also goes to every remembered server and, on rounds 0-5 and
 * every 4th after, to the whole /22 around one of server_ip / the
 * remembered servers (in turn): the PC's next DHCP lease is usually in
 * the same block as an old one. */
static bool DiscoverOnce(uint32_t* ip_be, uint16_t* port, int round) {
    SetState(MpState::Discovering);
    const bool verbose = round < 3 || round % 10 == 0;
    int u = SockSocket(MP_AF_INET, MP_SOCK_DGRAM, 0);
    if (u < 0) { WriteLog("[MP] discovery socket failed errno=%d\n", SockGetLastErrno()); return false; }
    MpSockAddrIn bindAddr = {};
    bindAddr.sin_len = sizeof(bindAddr);
    bindAddr.sin_family = MP_AF_INET;
    bindAddr.sin_addr = MP_INADDR_ANY;
    SockBind(u, &bindAddr, sizeof(bindAddr));
    int one = 1;
    SockSetSockOpt(u, MP_SOL_SOCKET, MP_SO_BROADCAST, &one, sizeof(one));
    int fl = SockFcntl(u, MP_F_GETFL, 0);
    if (fl >= 0) SockFcntl(u, MP_F_SETFL, fl | MP_O_NONBLOCK);

    uint32_t me_be = 0, mask_be = 0;
    if (!NifmGetCurrentIpConfig(&me_be, &mask_be) || me_be == 0) {
        mask_be = 0;
        NifmGetCurrentPrimaryIpAddress(&me_be);
    }
    const uint32_t me = __builtin_bswap32(me_be);
    uint32_t mask = __builtin_bswap32(mask_be);
    const int prefix = __builtin_popcount(mask);
    if (prefix < 22 || prefix > 30) mask = 0xffffff00u;
    const uint32_t net = me & mask, bcast = net | ~mask;
    uint32_t cfg = 0;
    if (!Config::mpServerIp.empty() &&
        SockInetPton(MP_AF_INET, Config::mpServerIp.c_str(), &cfg) != 1)
        cfg = 0;

    const char probe[] = "{\"t\":\"discover\",\"proto\":1}\n";
    MpSockAddrIn dst = {};
    dst.sin_len = sizeof(dst);
    dst.sin_family = MP_AF_INET;
    dst.sin_port = SockInetHtons((uint16_t)Config::mpDiscoveryPort);
    int sent = 0;
    auto sendTo = [&](uint32_t addr_be) {
        dst.sin_addr = addr_be;
        for (int t = 0; t < 4; t++) {
            if (SockSendTo(u, probe, sizeof(probe) - 1, 0, &dst, sizeof(dst)) >= 0) { sent++; return; }
            if (SockGetLastErrno() != MP_EAGAIN) return;
            SleepMs(2);
        }
    };
    bool found = false;
    auto poll = [&]() {
        while (!found) {
            char rbuf[1024];
            MpSockAddrIn src = {};
            uint32_t slen = sizeof(src);
            int64_t rn = SockRecvFrom(u, rbuf, sizeof(rbuf) - 1, 0, &src, &slen);
            if (rn <= 0) return;
            rbuf[rn] = 0;
            jsmntok_t tok[32];
            int ntok = mpjson::Parse(rbuf, (int)rn, tok, 32);
            int tIdx = ntok > 0 ? mpjson::Find(rbuf, tok, ntok, "t") : -1;
            char type[24] = "";
            if (tIdx >= 0) mpjson::Copy(rbuf, tok[tIdx], type, sizeof(type));
            int pIdx = ntok > 0 ? mpjson::Find(rbuf, tok, ntok, "port") : -1;
            int pnum = 0;
            if (pIdx >= 0) mpjson::ToInt(rbuf, tok[pIdx], &pnum);
            if (strcmp(type, "server") == 0 && pnum > 0 && pnum < 65536) {
                *ip_be = src.sin_addr;
                *port = (uint16_t)pnum;
                found = true;
            }
        }
    };

    sendTo(MP_INADDR_BROADCAST);
    sendTo(__builtin_bswap32(0x7f000001u));
    if (cfg) sendTo(cfg);
    if (me) {
        sendTo(__builtin_bswap32(bcast));
        for (uint32_t h = net + 1; h < bcast && !found; h++) {
            if (h == me) continue;
            sendTo(__builtin_bswap32(h));
            if ((h & 31) == 0) { SleepMs(1); poll(); }
        }
    }
    uint32_t blocks[1 + kMaxKnown];
    int nb = 0;
    auto addKnown = [&](uint32_t a_be) {
        const uint32_t h = __builtin_bswap32(a_be);
        if (!UsableIp(h) || found) return;
        if (a_be != cfg) sendTo(a_be);
        const uint32_t blk = h & 0xfffffc00u;
        if (me && blk <= bcast && net <= blk + 1023) return;   /* swept above */
        for (int i = 0; i < nb; i++)
            if (blocks[i] == blk) return;
        blocks[nb++] = blk;
    };
    if (cfg) addKnown(cfg);
    for (int i = 0; i < s_nKnown; i++) addKnown(s_knownIp[i]);
    uint32_t swept = 0;
    if (nb && !found && (round < 6 || round % 4 == 0)) {
        swept = blocks[s_sweepTurn++ % nb];
        for (uint32_t h = swept + 1; h < swept + 1023 && !found; h++) {
            if (h == me) continue;
            sendTo(__builtin_bswap32(h));
            if ((h & 31) == 0) { SleepMs(1); poll(); }
        }
    }
    for (int i = 0; i < 120 && !found; i++) {
        poll();
        if (!found) SleepMs(10);
    }
    SockClose(u);
    char sw[40] = "";
    if (swept)
        snprintf(sw, sizeof(sw), " + sweep %u.%u.%u.0/22", swept >> 24,
                 (swept >> 16) & 0xff, (swept >> 8) & 0xff);
    if (found || verbose)
        WriteLog("[MP] discovery #%d: me %u.%u.%u.%u/%d%s, %d probes -> %s %u.%u.%u.%u:%u\n",
                 round, me >> 24, (me >> 16) & 0xff, (me >> 8) & 0xff, me & 0xff,
                 __builtin_popcount(mask), sw, sent, found ? "server" : "none",
                 found ? (*ip_be & 0xff) : 0, found ? ((*ip_be >> 8) & 0xff) : 0,
                 found ? ((*ip_be >> 16) & 0xff) : 0, found ? (*ip_be >> 24) : 0,
                 found ? *port : 0);
    return found;
}

/* ------------------------------------------------------------------ */
/* TCP connect + session                                               */
/* ------------------------------------------------------------------ */

static bool ModeChanged(uint32_t gen) {
    return g_modeGen.load(std::memory_order_acquire) != gen;
}

static bool TcpConnect(uint32_t ip_be, uint16_t port, uint32_t timeoutMs = 3000) {
    SetState(MpState::Connecting);
    const uint32_t gen = g_modeGen.load();
    const int tries = (int)(timeoutMs / 50);
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

    for (int i = 0; i < tries && !ok; i++) {
        int r = SockConnect(g_sock, &sa, sizeof(sa));
        if (r == 0 || IsConnected()) {
            ok = true;
        } else if (IsInProgress()) {
            if (ModeChanged(gen)) break;
            SleepMs(50);
        } else {
            SetError("connect errno=%d", SockGetLastErrno());
            break;
        }
        if (i == tries - 1)
            SetError("connect timeout");
    }
    if (!ok) { CloseSock(); return false; }

    int one = 1;
    SockSetSockOpt(g_sock, MP_IPPROTO_TCP, MP_TCP_NODELAY, &one, sizeof(one));
    return true;
}

/* RFC 6455 opening handshake on the connected g_sock: GET path with
 * Upgrade: websocket, then wait (up to timeoutMs - a sleeping free-tier
 * server takes ~1 min to answer) for "HTTP/1.1 101". The response is read
 * byte by byte so no frame bytes are consumed with the header. */
static bool WsHandshake(const char* host, uint16_t port, const char* path,
                        uint32_t timeoutMs) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t raw[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = Rand32();
        memcpy(raw + i, &r, 4);
    }
    char key[25];
    int k = 0;
    for (int i = 0; i < 16; i += 3) {
        uint32_t v = (uint32_t)raw[i] << 16 | (i + 1 < 16 ? raw[i + 1] << 8 : 0) |
                     (i + 2 < 16 ? raw[i + 2] : 0);
        key[k++] = b64[(v >> 18) & 63];
        key[k++] = b64[(v >> 12) & 63];
        key[k++] = i + 1 < 16 ? b64[(v >> 6) & 63] : '=';
        key[k++] = i + 2 < 16 ? b64[v & 63] : '=';
    }
    key[k] = 0;
    char portSfx[8] = "";
    if (port != 80) snprintf(portSfx, sizeof(portSfx), ":%u", port);
    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s%s\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\nUser-Agent: MegaMixSwitchMod/1\r\n\r\n",
        path, host, portSfx, key);
    if (n <= 0 || n >= (int)sizeof(req) || !SendRaw(req, (size_t)n)) {
        SetError("upgrade request send failed");
        return false;
    }
    const uint32_t gen = g_modeGen.load();
    const uint64_t t0 = NowMs();
    char status[160];
    size_t sl = 0, total = 0;
    uint32_t tail = 0;
    while (true) {
        char c;
        int64_t rn = SockRecv(g_sock, &c, 1, 0);
        if (rn == 1) {
            if (sl < sizeof(status) - 1 && c != '\r' && c != '\n' && total == sl)
                status[sl++] = c;
            total++;
            tail = (tail << 8) | (uint8_t)c;
            if (tail == 0x0d0a0d0a) break;
            if (total > 16384) { SetError("upgrade: oversized response"); return false; }
            continue;
        }
        if (rn == 0) { SetError("server closed during upgrade"); return false; }
        if (!IsEagain()) { SetError("upgrade recv errno=%d", SockGetLastErrno()); return false; }
        if (ModeChanged(gen)) return false;
        if (NowMs() - t0 > timeoutMs) {
            SetError("no answer from %s in %us", host, (unsigned)(timeoutMs / 1000));
            return false;
        }
        SleepMs(20);
    }
    status[sl] = 0;
    int code = 0;
    sscanf(status, "HTTP/%*d.%*d %d", &code);
    WriteLog("[MP] upgrade %s%s%s -> '%s' after %llu ms\n", host, portSfx, path,
             status, (unsigned long long)(NowMs() - t0));
    if (code != 101) {
        s_lastHttpCode = code;
        SetError("server answered HTTP %d (not a WebSocket)", code);
        return false;
    }
    s_lastHttpCode = 0;
    s_ws = true;
    WsDecReset();
    return true;
}

static bool SayHello() {
    char escName[64], escId[24];
    mpjson::EscapeInto(escName, sizeof(escName), Config::mpPlayerName.c_str());
    mpjson::EscapeInto(escId, sizeof(escId), Config::mpClientId.c_str());
    char line[256];
    snprintf(line, sizeof(line),
             "{\"t\":\"hello\",\"proto\":1,\"mod\":\"%s\",\"game\":\"mm+1.03\",\"plat\":\"pc\",\"cid\":\"%s\",\"name\":\"%s\"}\n",
             "pc-0.1.0-dev+" __DATE__ " " __TIME__, escId, escName);
    if (!SendAll(line, strlen(line))) { SetError("hello send failed"); return false; }

    /* poll-free: recv every 10ms up to 5s until a full line arrives */
    char rbuf[512];
    size_t rl = 0;
    bool gotLine = false;
    for (int i = 0; i < 500 && !gotLine; i++) {
        int64_t rn = RecvPayload(rbuf + rl, sizeof(rbuf) - 1 - rl);
        if (rn > 0) {
            rl += (size_t)rn;
            rbuf[rl] = 0;
            if (strchr(rbuf, '\n')) gotLine = true;
        } else if (rn == 0) {
            SetError("welcome: peer closed"); return false;
        } else if (rn != -2) {
            SetError("welcome recv errno=%d", SockGetLastErrno());
            return false;
        }
        SleepMs(10);
    }
    if (!gotLine) { SetError("welcome timeout"); return false; }
    /* parse only the first line: a reconnect resync can share the buffer */
    int firstLen = (int)(strchr(rbuf, '\n') - rbuf);

    jsmntok_t tok[48];
    int ntok = mpjson::Parse(rbuf, firstLen, tok, 48);
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
    ClearBlob();
    s_lastRecv = NowMs();
    s_lastPingSent = 0;
    uint64_t nextPing = NowMs() + 2000;
    const uint32_t gen = g_modeGen.load();

    while (true) {
        FlushOutgoing();      /* first: a lobby_leave queued by SetServerMode */
        if (ModeChanged(gen)) {
            WriteLog("[MP] server mode changed, closing session\n");
            return;
        }

        /* drain recv until EAGAIN */
        char rbuf[4096];
        while (true) {
            int64_t rn = RecvPayload(rbuf, sizeof(rbuf));
            if (rn > 0) {
                s_lastRecv = NowMs();
                for (int64_t i = 0; i < rn; i++) {
                    char c = rbuf[i];
                    if (c == '\n') {
                        HandleLine(s_lineBuf, (int)s_lineLen);
                        s_lineLen = 0;
                    } else if (s_lineLen >= sizeof(s_lineBuf)) {
                        WriteLog("[MP] line overflow, dropping conn\n");
                        return;
                    } else {
                        s_lineBuf[s_lineLen++] = c;
                    }
                }
            } else if (rn == 0) {
                WriteLog("[MP] server closed (recv=0)\n");
                return;
            } else if (rn == -2) {
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

static const char* ModeName(ServerMode m) {
    return m == kServerOff ? "off" : m == kServerCloud ? "cloud" : "local";
}

static void NetThread(void*) {
    /* Off: no network request, no sockets until a server is chosen */
    if (GetServerMode() == kServerOff) {
        SetState(MpState::Disabled);
        WriteLog("[MP] server mode: off (idle)\n");
        while (GetServerMode() == kServerOff) SleepMs(200);
    }
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

    /* Each pass: the address of the last session (this boot), then the
     * configured server_ip, then a discovery round (which also probes the
     * servers remembered from earlier boots and the blocks around them).
     * A stale server_ip (another network) therefore costs one failed
     * connect, not forever. */
    LoadKnownServers();
    uint32_t cfgIp = 0, lastIp = 0;
    uint16_t lastPort = 0;
    if (!Config::mpServerIp.empty() &&
        SockInetPton(MP_AF_INET, Config::mpServerIp.c_str(), &cfgIp) != 1) {
        WriteLog("[MP] bad server_ip '%s'\n", Config::mpServerIp.c_str());
        cfgIp = 0;
    }
    const uint16_t cfgPort = (uint16_t)Config::mpServerPort;
    auto attempt = [&](uint32_t ip_be, uint16_t port) {
        char ab[64];
        snprintf(ab, sizeof(ab), "%u.%u.%u.%u:%u",
                 ip_be & 0xff, (ip_be >> 8) & 0xff, (ip_be >> 16) & 0xff,
                 ip_be >> 24, port);
        SetServer(ab);
        if (!TcpConnect(ip_be, port) || !SayHello()) {
            CloseSock();
            return false;
        }
        backoff = 0;
        lastIp = ip_be; lastPort = port;
        RememberServer(ip_be, port);
        SessionLoop();
        CloseSock();
        return true;
    };
    /* Online server: resolve cloud_host, TCP to cloud_port, WebSocket
     * upgrade on /switch (both waits up to kCloudTimeoutMs for a sleeping
     * free-tier server), then the same hello/session as on the LAN. */
    auto attemptCloud = [&]() {
        char host[sizeof(s_cloudHost)], path[sizeof(s_cloudPath)];
        uint16_t port;
        {
            std::lock_guard<std::mutex> lk(g_statusMtx);
            memcpy(host, s_cloudHost, sizeof(host));
            memcpy(path, s_cloudPath, sizeof(path));
            port = s_cloudPort;
        }
        char ab[64];
        snprintf(ab, sizeof(ab), "%s:%u", host, port);
        SetServer(ab);
        SetState(MpState::Connecting);
        uint32_t ip_be = 0;
        if (!SockResolveIpv4(host, &ip_be)) {
            SetError("can't resolve %s (DNS)", host);
            return false;
        }
        WriteLog("[MP] online server %s -> %u.%u.%u.%u:%u\n", host, ip_be & 0xff,
                 (ip_be >> 8) & 0xff, (ip_be >> 16) & 0xff, ip_be >> 24, port);
        if (!TcpConnect(ip_be, port, kCloudTimeoutMs) ||
            !WsHandshake(host, port, path, kCloudTimeoutMs) || !SayHello()) {
            CloseSock();
            return false;
        }
        backoff = 0;
        s_rateLimitMs = 0;          /* connected: drop any 429 backoff */
        SessionLoop();
        CloseSock();
        return true;
    };
    int round = 0;
    uint32_t gen = g_modeGen.load();
    WriteLog("[MP] server mode: %s\n", ModeName(GetServerMode()));
    while (true) {
        if (ModeChanged(gen)) {
            gen = g_modeGen.load();
            backoff = 0;
            WriteLog("[MP] server mode: %s\n", ModeName(GetServerMode()));
        }
        if (GetServerMode() == kServerOff) {
            SetState(MpState::Disabled);
            while (!ModeChanged(gen)) SleepMs(200);
            continue;
        }
        if (IsCloudMode()) {
            if (!attemptCloud() && !ModeChanged(gen)) SetState(MpState::Error);
        } else {
            bool done = lastIp && attempt(lastIp, lastPort);
            if (!done && cfgIp && !(cfgIp == lastIp && cfgPort == lastPort) && !ModeChanged(gen))
                done = attempt(cfgIp, cfgPort);
            uint32_t ip_be = 0;
            uint16_t port = 0;
            if (!done && !ModeChanged(gen) && DiscoverOnce(&ip_be, &port, round++) &&
                !ModeChanged(gen)) {
                if (cfgIp && ip_be != cfgIp)
                    WriteLog("[MP] server_ip %s unreachable, using discovered %u.%u.%u.%u:%u\n",
                             Config::mpServerIp.c_str(), ip_be & 0xff, (ip_be >> 8) & 0xff,
                             (ip_be >> 16) & 0xff, ip_be >> 24, port);
                attempt(ip_be, port);
            }
        }
        uint32_t wait;
        if (IsCloudMode() && s_lastHttpCode == 429) {
            /* rate limited: 5 s doubling to 120 s (resets on a successful
             * connect); a normal failure falls back to backoffSeq but the
             * ratchet survives until the next 429 or connect */
            s_rateLimitMs = s_rateLimitMs < 5000 ? 5000
                : (s_rateLimitMs > 60000 ? 120000 : s_rateLimitMs * 2);
            wait = s_rateLimitMs;
            WriteLog("[MP] HTTP 429 rate limited - retry in %u s\n",
                     (unsigned)(wait / 1000));
        } else {
            wait = backoffSeq[backoff < 3 ? backoff++ : 3];
        }
        s_lastHttpCode = 0;
        for (uint32_t t = 0; t < wait && !ModeChanged(gen); t += 100) SleepMs(100);
    }
}

static void Reconnect() {
    g_modeGen.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lk(g_statusMtx);
    g_lastError[0] = 0;
}

void SetServerMode(ServerMode m) {
    if (m > kServerCloud || GetServerMode() == m) return;
    lobby::LeaveForServerSwitch();                   /* rooms don't move servers */
    Config::mpServerMode = ModeName(m);
    Config::Save();
    g_mode.store(m, std::memory_order_release);
    Reconnect();
    /* same banners the Switch OPTIONS picker raises on a mode switch */
    lobby::SystemBanner(m == kServerOff   ? "Multiplayer off"
                      : m == kServerLocal ? "Local server - searching the network..."
                                          : "Online server - connecting...");
    WriteLog("[MP] server mode -> %s\n", ModeName(m));
}

/* parse into the net thread's copy; false (nothing stored) if invalid */
static bool ApplyCloudUrl(const char* url) {
    char host[sizeof(s_cloudHost)], path[sizeof(s_cloudPath)];
    uint16_t port = 80;
    if (!ParseCloudUrl(url, host, sizeof(host), &port, path, sizeof(path))) return false;
    std::lock_guard<std::mutex> lk(g_statusMtx);
    memcpy(s_cloudHost, host, sizeof(host));
    memcpy(s_cloudPath, path, sizeof(path));
    s_cloudPort = port;
    return true;
}

bool SetCloudUrl(const char* url) {
    char t[160];
    snprintf(t, sizeof(t), "%s", url ? url : "");
    char* b = t;
    while (*b == ' ') b++;
    for (size_t n = strlen(b); n && b[n - 1] == ' '; n--) b[n - 1] = 0;
    if (!*b) {
        snprintf(t, sizeof(t), "%s", kDefaultCloudUrl);
        b = t;
    }
    if (!ApplyCloudUrl(b)) {
        WriteLog("[MP] bad online server address '%s'\n", b);
        lobby::SystemBanner("Invalid server address - use host[:port][/path]");
        return false;
    }
    if (Config::mpCloudUrl != b) {
        Config::mpCloudUrl = b;
        Config::Save();
        WriteLog("[MP] online server -> %s\n", b);
        if (IsCloudMode()) {
            lobby::LeaveForServerSwitch();
            Reconnect();
            lobby::SystemBanner("Online server changed - connecting...");
        }
    }
    return true;
}

void Init() {
    g_mode.store(Config::mpServerMode == "cloud" ? kServerCloud
                 : Config::mpServerMode == "off" ? kServerOff : kServerLocal);
    if (!ApplyCloudUrl(Config::mpCloudUrl.c_str())) {
        WriteLog("[MP] bad cloud_url '%s', using %s\n", Config::mpCloudUrl.c_str(),
                 kDefaultCloudUrl);
        ApplyCloudUrl(kDefaultCloudUrl);
    }
    lobby::Init();
    if (!ApiResolveAll()) {
        WriteLog("[MP] disabled: unresolved symbols\n");
        SetState(MpState::Error);
        SetError("missing symbols");
        return;
    }
    SetMpEnabled(true);

    static std::thread s_thread;
    s_thread = std::thread(NetThread, nullptr);
    s_thread.detach();
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
    float    pct, border, border_eff;
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
        "\"ps\":%u,\"ip\":%u,\"iv\":%u,\"pe\":%u,\"res\":%d,\"st\":%llu,"
        "\"bd\":%.2f,\"bde\":%.2f}",
        s_frame, cur.loaded, cur.paused, cur.byte2, cur.score, cur.combo,
        cur.max_combo, cur.life, (double)cur.pct,
        cur.judge[0], cur.judge[1], cur.judge[2], cur.judge[3], cur.judge[4],
        cur.scoring, cur.type, cur.diff, cur.pv_id,
        cur.ps, cur.ip, cur.iv, cur.pe, cur.res,
        (unsigned long long)st, (double)cur.border, (double)cur.border_eff);
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
    cur.border   = pv->clear_border;
    cur.border_eff = pv->clear_border_eff;
    cur.scoring  = pv->scoring_enabled;
    memcpy(cur.judge, pv->judge_count, sizeof(cur.judge));

    PvGameplayInfo* gi = GetPvGameplayInfo();
    cur.type = gi ? gi->type : -1;
    cur.diff = gi ? gi->difficulty : -1;
    auto* lp = game::GetPVLoadParam();
    cur.pv_id = lp ? lp->data[lp->data[0].int8].pv_id : -1;

    /* extra state flags: Switch-only main-module offsets, not ported (0) */
    cur.ps = 0;   /* pause state */
    cur.ip = 0;   /* is_play */
    cur.iv = 0;   /* is_pv */
    cur.pe = 0;   /* pv_end */
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
    avatar::GamePump();

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
