#pragma once

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
    char     server[64];     /* "ip:port" */
    int      rtt_ms;         /* last ping RTT, -1 unknown */
    char     last_error[96];
    uint32_t connect_count;
};

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

} // namespace mp
