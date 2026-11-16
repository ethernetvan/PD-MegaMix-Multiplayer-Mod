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
};

/* Server selection: false = Local (LAN search / server_ip, raw TCP),
 * true = the online server Config::mpCloudHost over a WebSocket on plain
 * HTTP (ws://host:port/switch). SetCloudMode is for the game thread: it
 * saves config.toml, leaves a room on the old server and makes the net
 * thread drop the current session/search and reconnect in the new mode. */
bool IsCloudMode();
void SetCloudMode(bool cloud);
const char* CloudHost();
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
