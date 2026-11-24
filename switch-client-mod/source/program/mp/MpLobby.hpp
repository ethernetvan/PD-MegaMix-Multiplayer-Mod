#pragma once
#include "MpAvatar.hpp"

#include <cstdint>
#include "../nc/diva_nc.hpp"

struct ImDrawList;
struct ImFont;

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
void DrawGaugeRace();
void DrawRankGlow();
void TriggerRankGlow(bool up);   /* dbg_cmd `glow up|down` */
void DrawCountdown();
void DrawPopups();
struct Popup { char kind[16], cid[20], name[40]; int32_t value;
               uint64_t bornAt; };
const Popup* Popups(int* count);
void PushPopup(const char* kind, const char* cid, const char* name,
               int32_t value);
void PopupTest();                /* dbg_cmd `popup_test` */
int  NotifyStyle();              /* 0 avatars, 1 text, 2 off */
void SetNotifyStyle(int st);
void SystemBanner(const char* text);  /* always shows (not notify_style) */
/* opt-in: show portraits other consoles upload for modules this console
 * lacks (Config::mpRemoteAvatars, saved) */
void SetRemoteAvatars(bool on);
bool InLobby();
bool PanelWanted();           /* song select + connected */
void DrawPanel();
void DrawBanners();
void DrawLeaderboard();
void DrawWaiting();
void DrawStandings();
void DrawResultsAvatars();
void DrawEmergency();

/* Commands from panel buttons / dbg_cmd. */
void CmdLobbyCreate();
void CmdLobbyJoin(const char* code);
void CmdLobbyLeave();
/* the console switches servers: tell the old one, leave the room locally */
void LeaveForServerSwitch();
void CmdSetName(const char* name);

/* dbg_cmd sel_lists: dumps the 8 sorted_pv_lists of the current sort
 * mode on the next pv_sel frame as a dbg_sel_lists message. */
void DbgSelLists(const int32_t* pvs, int n);

/* which version of the mod a member runs (lobby `plat`) */
enum { PLAT_UNKNOWN = 0, PLAT_SWITCH = 1, PLAT_PC = 2 };

/* For MpUi: snapshot getters (copies; safe to call on UI thread). */
struct MemberInfo { char cid[20]; char name[40]; bool connected, is_leader,
                    ready; char reason[24]; char role[12]; int32_t score; float pct;
                    avatar::Ref av; /* Shared Set A slot 1 module (-1 unknown) + chara/mh/img */
                    /* room session ranking: 1-based position (0 unknown),
                     * wins, and times ranked 1st / 2nd / 3rd */
                    int32_t srPos, wins, places[3];
                    int plat;            /* PLAT_* */
                    int diff, ed;        /* difficulty 0..3 (-1 unknown), EXTRA edition */
                    bool nosong;         /* doesn't have the picker's current song */
                    };
struct Snapshot {
    int      phase;           /* LPhase */
    bool     connected;
    int32_t  offlineMs;       /* ms since the session dropped, -1 online */
    bool     inSongSelect;    /* pv_sel is active */
    bool     inSong;          /* a song is loaded and playing/paused (scene "game") */
    bool     topRightBusy;    /* an active note target/button is near top-right */
    bool     waitingStart;    /* loaded, waiting for the round's start (any member) */
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
    char     unavailFor[96];  /* names the leader's pick isn't available for */
    /* board rows */
    struct { char cid[20]; char name[40]; int32_t score; float pct; int32_t life;
             int rank; bool connected, me; } board[8];
    int      boardCount;
    float    myPct;           /* local fill, same value the gauge shows */
    int32_t  sinceStartMs;    /* ms since the round's synced start, -1 none */
    int32_t  countdownMs;     /* ms left in the pre-start countdown, -1 none */
    int32_t  countdownAgeMs;  /* ms since the countdown began, -1 none */
    int32_t  goMs;            /* ms since the start, -1 when > 800 or none */
    bool     standingsComplete;
    struct { char cid[20]; char name[40]; int32_t score; float pct; int32_t max_combo;
             int32_t life; int rank; bool final; bool me; bool connected;
             int diff, ed;    /* the chart this player was on (-1 unknown) */
             int32_t jc[5];   /* COOL GOOD SAFE BAD MISS of the final */
             bool hasJc;
             float border;    /* that console's clear % (0 unknown) */
             int clear;       /* its verdict: 1 clear, 0 not, -1 unknown */
             bool drop, autoplay;
             int plat;        /* PLAT_* */
             avatar::Ref av; } standing[8];
    int      standingCount;
    int32_t  resultsAgeMs;  /* ms on the results screen, -1 when not shown */
    /* banners */
    struct { char text[128]; float ttl; } banners[4];
    int      bannerCount;
    int      round;
    char     ownerCid[20];
    char     pickMode[12];
    char     assignedCid[20];
    bool     groupPaused;
    char     voteKind[8];
    int      voteAgreed, voteNeed, voteEndsIn;
    bool     minusHeld;
    int      minusHeldMs;
    bool     sameDiff;        /* room option: everyone plays the picker's difficulty */
    float    gaugeTick[3];    /* % of the gauge's BORDER / G / E ticks, < 0 none */
    /* expanded results panel: 0 none, 1 armed (A opens), 2 open, 3 closed
     * (Y reopens); ms since it was opened / closed (-1 never) */
    int      resPanel, resPage, resPerPage;
    int32_t  resPanelMs;
    bool     resCanReopen;
    int      myDiff, myEd;    /* this console's song-select difficulty */
};

/* group pause / pick mode commands */
void CmdPickMode(const char* mode, const char* cid);
void CmdKick(const char* cid);       /* owner only */
void CmdSameDiff(bool on);           /* owner only: room option same_diff */
/* Song select: the "doesn't have this song" tag, right edge at rx, top at
 * y (s = 720p scale); returns the height it used, 0 when nobody lacks it. */
float DrawNoSongTag(ImDrawList* dl, ImFont* font, float rx, float y, float s);
void CmdPauseReq();
void CmdResumeReq();
bool OverlayOpen();
void ToggleOverlay();
void SetOverlayOpen(bool open);
int64_t OverlayClosedAt();   /* ms of last close, -1 none */
/* true while the overlay is open AND driven by pad buttons (focus model);
 * ImGuiOverlay then leaves the stick/ZL/ZR alone. */
bool PadModeActive();
/* call before SetOverlayOpen(true) to start in pad mode (e.g. ZR open) */
void RequestPadMode();
void ReportAutoPlay(bool on);
void GetSnapshot(Snapshot* out);

} // namespace mp::lobby
