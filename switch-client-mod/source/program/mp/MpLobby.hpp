#pragma once

#include <cstdint>
#include "../nc/diva_nc.hpp"

namespace mp::lobby {

void Init();

/* Feed a raw inbound line (no \n) — net thread only. */
void PushLine(const char* line, int len);

/* Parse inbound queue + run timers — call every frame on the game thread
 * (from mp::OnFrame, top of nvnImguiCalc). */
void Pump();

/* Game-thread hook entry points. */
void OnPvSelInit();
void OnPvSelDest();
void OnPvSelFrame(const void* sel);
void OnPvGameFrame(PVGameData* pv);
void OnStageResult(bool init);

/* UI visibility helpers + draws (ImGui thread = same as Pump thread). */
bool InLobby();
bool PanelWanted();           /* song select + connected */
void DrawPanel();
void DrawBanners();
void DrawLeaderboard();
void DrawWaiting();
void DrawStandings();

/* Commands from panel buttons / dbg_cmd. */
void CmdLobbyCreate();
void CmdLobbyJoin(const char* code);
void CmdLobbyLeave();
void CmdSetName(const char* name);

/* For MpUi: snapshot getters (copies; safe to call on UI thread). */
struct MemberInfo { char cid[20]; char name[40]; bool connected, is_leader,
                    ready; char reason[24]; char role[12]; int32_t score; float pct; };
struct Snapshot {
    int      phase;           /* LPhase */
    bool     connected;
    char     code[8];
    char     myCid[20];
    char     leaderCid[20];
    int      memberCount;
    MemberInfo members[8];
    int32_t  selPv;
    char     selName[48];
    int      loadedCount, playerCount;
    char     statusLine[96];
    char     notReady[128];   /* leader warnings */
    /* board rows */
    struct { char name[40]; int32_t score; float pct; bool connected, me; } board[8];
    int      boardCount;
    bool     standingsComplete;
    struct { char name[40]; int32_t score; float pct; int32_t max_combo;
             int32_t life; int rank; bool final; bool me; } standing[8];
    int      standingCount;
    /* banners */
    struct { char text[128]; float ttl; } banners[4];
    int      bannerCount;
    int      round;
};
void GetSnapshot(Snapshot* out);

} // namespace mp::lobby
