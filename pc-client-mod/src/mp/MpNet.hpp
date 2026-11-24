#pragma once
#include <cstddef>

#include <cstdint>

namespace mp {

enum class MpState : uint8_t {
    Disabled = 0,
    WaitingNetwork,
    SocketInit,
    Discovering,
    Connecting,
    Connected,
    Error,
};

struct MpStatus {
    MpState  state;
    char     server[64];     /* "ip:port" or "host:port" (online) */
    int      rtt_ms;         /* last ping RTT, -1 unknown */
    char     last_error[96];
    uint32_t connect_count;
    bool     cloud;          /* online server mode (WebSocket) */
    uint8_t  mode;           /* ServerMode */
};

/* Server selection (Config::mpServerMode):
 *   Off   - never connects (no network request, no sockets, no search)
 *   Local - LAN search / server_ip, raw TCP
 *   Cloud - the online server Config::mpCloudUrl, WebSocket over plain
 *           HTTP (ws://host[:port]/switch)
 * SetServerMode / SetCloudUrl are for the game thread: they save
 * config.toml, leave a room on the old server and make the net thread drop
 * the current session/search and reconnect in the new mode. */
enum ServerMode : uint8_t { kServerOff = 0, kServerLocal = 1, kServerCloud = 2 };
ServerMode GetServerMode();
void SetServerMode(ServerMode m);
bool IsCloudMode();
/* host[:port][/path], optional ws:// or http:// (https/wss map to plain
 * port 80: the console has no TLS); empty = the default server. False if
 * it can't be parsed (nothing changes). */
bool SetCloudUrl(const char* url);
const char* CloudUrl();          /* game thread: as shown/saved */
constexpr const char* kDefaultCloudUrl = "mikumod.evanthomas.dev";
/* online mode: a free-tier server may need ~1 min to wake up */
constexpr uint32_t kCloudTimeoutMs = 65000;

/* Called from MainHook once (only if Config::mpEnabled). Spawns the
 * MpNet thread. Never blocks the caller. */
void Init();

/* Enqueue one NDJSON line (no '\n' needed) for the server. Safe from any
 * thread; never allocates; drops/overwrites oldest when full. */
void Enqueue(const char* line);

/* Game-thread command pump; call at the very top of nvnImguiCalc. */
void OnFrame();

/* Telemetry snapshot hook; call every PV-game frame after Orig. */
void OnPvGameFrame();

/* Song-select telemetry; call every selector frame after Orig with the
 * selector pointer, and on init/dest. */
void OnPvSelFrame(const void* sel);
void OnPvSelInit();
void OnPvSelDest();

/* Results telemetry; init=true on Init hook, false on Dest hook. */
void OnStageResult(bool init);

MpState GetState();
void    GetStatus(MpStatus* out);
const char* StateName(MpState s);

/* big one-shot line (avatar_img) - a static 64 KiB slot outside the
 * 510-byte ring; false if busy, not connected, or too big */
bool EnqueueBlob(const char* line, size_t len);

} // namespace mp
