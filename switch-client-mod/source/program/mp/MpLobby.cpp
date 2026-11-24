#include "MpLobby.hpp"
#include "MpAvatar.hpp"
#include "MpNet.hpp"
#include "MpJson.hpp"
#include "MpInput.hpp"
#include "MpMenu.hpp"
#include "../Config.hpp"
#include "../logger.hpp"
#include "../overlays/SongQuickJump.hpp"
#include "../nc/ui/customize_sel.hpp"
#include "../nc/ui/pv_sel.hpp"
#include "../hid.hpp"
#include "../AutoPlay.hpp"
#include <nn.hpp>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <deque>
#include <string>

extern PVGameArcade* g_current_game;

namespace mp::lobby {

/* ------------------------------------------------------------------ */
enum LPhase {
    L_NONE, L_SELECT, L_LAUNCHING, L_LOADING, L_PLAYING,
    L_SPECTATING, L_DROPPED, L_RESULTS
};

static std::recursive_mutex s_mtx;
static LPhase   s_phase = L_NONE;
static char     s_myCid[24] = "";
static char     s_myName[40] = "";
static char     s_code[8] = "";
static char     s_serverPhase[12] = "";
static char     s_leaderCid[24] = "";
static int32_t  s_round = 0;
static int32_t  s_selPv = -1;
static char     s_selName[48] = "";

struct MRec {
    char cid[24]; char name[40];
    bool connected; char role[12];
    bool loaded, ready; char reason[24];
    int32_t score; float pct;
    int32_t max_combo; int32_t life; int rank; bool final;
    avatar::Ref av;
    int32_t sr[5];   /* session ranking: pos, wins, 1st, 2nd, 3rd places */
    int8_t  plat;    /* PLAT_* (which version of the mod) */
    int8_t  diff, ed;/* difficulty 0..3 (-1 unknown), 1 = EXTRA edition */
    bool    nosong;  /* its console doesn't have the picker's song */
};
static MRec s_members[8];
static int  s_memberCount = 0;

static bool  s_connected = false;
static int64_t s_offlineSince = 0;      /* ms the session dropped, 0 = online */
static bool  s_leaveOnReconnect = false;  /* left locally while offline */
static int64_t s_ignoreRoomUntil = 0;   /* drop the stale room resync */
static bool  s_amLeader  = false;
static bool  s_amPlayer  = false;
static bool  s_committed = false;
static bool  s_selActive = false;
static bool  s_selBrowsing = false;  /* song list up: state 6, no picker/customize */
static int32_t s_leaderPv = -1;
static int32_t s_localPv  = -1;
static int32_t s_launchHandledRound = -1;
static bool  s_launchBlocking = false;
static int   s_lastSelState = -1;
static int32_t s_lastSentSel = -1;
static int64_t s_lastSelMs = 0;
static int64_t s_lastJumpMs = 0;
static int32_t s_lastJumpTarget = -1;
static int   s_jumpVerify = 0;
static bool  s_jumpFailedDiff = false;
static char  s_ackSig[120] = "";
static char  s_unavailFor[96] = "";    /* names the pick isn't available for */
/* room option (owner): everyone plays the song picker's difficulty */
static bool    s_sameDiff = false;
/* room option (owner): the game's No Fail Mode for everyone. -1 = the
 * server has no such rule (the game's own setting stays), else 0 / 1;
 * s_roundNoFail = what the running round was launched with */
static int     s_noFail = -1, s_roundNoFail = -1;
/* this console's song-select difficulty, and what the server was told */
static int32_t s_myD = -1, s_myE = 0;
static int32_t s_sentD = -2, s_sentE = 0;
static int64_t s_diffSentAt = 0;
/* same_diff: the picker's (d,e) the follow target was resolved against
 * (-1 = not forced), the last fallback banner, and the difficulty-only
 * sync used while the picker has no song up */
static int32_t s_folWantD = -1, s_folWantE = 0;
static int32_t s_folDiffBanner[3] = {-1, -1, -1};
static int     s_diffSyncTries = 0;
static int32_t s_diffSyncSig = -1;
static int64_t s_diffSyncAt = 0;
static int     s_launchDiffTries = 0;
/* expanded results panel (A on the results screen while in a room):
 * 0 not on results, 1 armed (the next A opens it instead of reaching the
 * game), 2 open (everything but Minus is ours), 3 closed again (A goes
 * to the game, Y reopens) */
static int     s_resPanel = 0;
static int     s_resPage = 0;
static int64_t s_resPanelAt = 0;     /* ms of the last open/close, 0 none */
static const int  kResPerPage = 4;      /* players per page */
/* the game's Retry / Return menu can't be shielded from the keyboard on
 * every platform: where it can't, the panel only reopens before it */
static const bool kResReopenOnMenu = true;
static int32_t s_slPvs[24];
static int   s_slN = 0;
static bool  s_slPending = false;
static int32_t s_folLeaderPv = -1;   /* leader pv the target was resolved for */
static int32_t s_folTarget = -1;     /* resolved jump target (-1 none) */
static bool    s_folMissing = false; /* not selectable at any difficulty */
static int32_t s_folTD = -1, s_folTE = -1;  /* (diff,ed) to jump to */
static int     s_folFail = 0;        /* failed verify attempts, this target */
static bool    s_folGaveUp = false;
static int32_t s_folLastD = -1, s_folLastE = -1;
static int64_t s_launchStartMs = 0;
static int   s_launchStage = 0;
static int   s_launchTries = 0;
static int64_t s_launchStageAt = 0;
static bool  s_apActive = false;
static int64_t s_apT0 = 0, s_apLastTap = 0;
static bool  s_loadedSent = false;
static bool  s_startSeen = false;
static bool  s_resumePending = false;
static int   s_resumeTries = 0;
static int64_t s_resumeLastMs = 0;
static bool  s_standingsComplete = false;
static bool  s_backInSelSent = false;
static int64_t s_resultInitAt = 0;
static int64_t s_resultsShownAt = 0;   /* ms when the results screen appeared */
static bool  s_quitting = false;         /* Return-to-Song-List from pause */
static int64_t s_lastPauseReqMs = 0;
static int64_t s_recLastTap = 0;         /* last pause reconciler tap */
static int   s_recTaps = 0;              /* consecutive reconciler taps */
static bool  s_recTarget = false;        /* last reconcile target */
static int64_t s_blockBannerAt = 0;      /* rate limit for swallow banners */
static int   s_prevPvPaused = 0;
/* results auto-return driver */
static bool  s_retBannered = false;
static int64_t s_driveLastTap = 0;
static int   s_scoreTries = 0;
/* retry watch: armed on a results dest in a guarded room; a song clock
 * that keeps running with no pv_sel init = the user hit Retry anyway */
static bool  s_retryWatch = false;
static uint64_t s_retryStLast = 0;
static uint64_t s_retryStChangeAt = 0;
static int   s_retryRecStage = 0;        /* 0 off; 1 pause, 2 nav, 3 A, 4 sel */
static int   s_retryRecTries = 0;
static int64_t s_retryRecLastTap = 0;
static int64_t s_retryRecT0 = 0;
static int   s_retryRecRounds = 0;
static char  s_scene[12] = "other";
static int32_t s_playedRound = 0;        /* round actually played this boot */
static int32_t s_finalSentRound = 0;
/* the last final we sent: resent after a reconnect, since a session that
 * drops right after sending it can lose it (seen on the online server) */
static char    s_finalLine[320] = "";
static int32_t s_finalLineRound = 0;
static bool  s_roundOver = false;        /* round_end seen */
static bool  s_autoReturn = false;       /* drive results->song-list later */
static int32_t s_standingsRound = 0;
static int64_t s_stateLastMs = 0;
static bool  s_statePending = false;
static int64_t s_destCheckAt = -1;
static int64_t s_minusHeldAt = -1;
static bool  s_expectLobby = false;
static int64_t s_expectLobbyAt = 0;
static char  s_statusLine[96] = "";
static char  s_notReady[128] = "";

static int32_t s_curScore = 0, s_curCombo = 0, s_curLife = 0;
static float   s_curPct = 0.0f;
static float   s_curBorder = 0.0f;   /* the song's clear %, from the game */
/* where the game's gauge ticks stand right now, in %: BORDER (the running
 * border), G and E (running border x great|excellent % / clear %) */
static float   s_gaugeTick[3] = { -1.0f, -1.0f, -1.0f };
/* game fns: the GREAT / EXCELLENT % of the current difficulty (tables at
 * 0xb12774 / 0xb12760, read by the gauge's tick placement at 0x689530) */
static inline FUNCTION_PTR(float, __fastcall, GaugeGreatPct, 0x17a410);
static inline FUNCTION_PTR(float, __fastcall, GaugeExcellentPct, 0x17a440);
static bool    s_topRightBusy = false;
static int32_t s_lastSentScore = -1, s_lastSentCombo = -1,
               s_lastSentLife = -1;
static float   s_lastSentPct = -1.0f;

struct Banner { char text[128]; float ttl; };
static Banner s_banners[4];
static int  s_bannerCount = 0;

struct BoardRow { char cid[24]; char name[40]; int32_t score; float pct;
                  int32_t life; int rank; bool connected; };
static BoardRow s_board[8];
static int  s_boardCount = 0;
struct StandingRow { char cid[20]; char name[40]; int32_t score; float pct;
                     int32_t max_combo; int32_t life; int rank; bool final;
                     bool connected; int32_t diff, ed;
                     int32_t jc[5]; bool hasJc; float border;
                     int8_t clear;      /* console verdict: 1 / 0, -1 unknown */
                     bool drop, autoplay; };
static StandingRow s_standings[8];
static int  s_standingCount = 0;

static bool  s_ready = true;
static char  s_readyReason[24] = "";
static int32_t s_readyPv = -1;
static bool  s_readyDirty = false;

static char  s_ownerCid[24] = "";
static char  s_pickMode[12] = "rotate";
static char  s_assignedCid[24] = "";
static bool  s_groupPaused = false;
static bool  s_gamePaused = false;
static char  s_voteKind[8] = "";
static int   s_voteAgreed = 0, s_voteNeed = 0, s_voteEndsIn = 0;
static bool  s_voteExpired = false;
static bool  s_overlayOpen = false;
/* the last room join (Snapshot.join*): the server answers a good one
 * with `lobby`, a bad one with `lobby_error`, a lost one not at all */
static int   s_joinState = JOIN_IDLE;
static char  s_joinCode[8] = "";
static char  s_joinErr[72] = "";
static int64_t s_joinAt = 0;
static bool  s_joinQuiet = false;     /* asked from the join screen */
static const int64_t kJoinTimeoutMs = 6000;
/* the room's web key (web_key reply to CmdWebKey), owner only */
static char  s_webKey[16] = "";
static char  s_webUrl[72] = "";
static int64_t s_ovlClosedAt = -1;   /* ms; suppresses ZR re-open in Hints */

struct InSlot { uint16_t len; char data[4094]; };
static InSlot   s_inQ[32];
static uint32_t s_inHead = 0;
static uint32_t s_inCount = 0;
static std::mutex s_inQMtx;

static bool    s_inLobbyDirty();
static void    ApplyBlocks();

static uint64_t NowMs() {
    return (uint64_t)nn::os::GetSystemTick().GetInt64Value() * 1000
           / nn::os::GetSystemTickFrequency();
}

static void SendLine(const char* m) { mp::Enqueue(m); }

static void ReportScene() {
    char m[64];
    snprintf(m, sizeof(m), "{\"t\":\"scene\",\"where\":\"%s\"}", s_scene);
    SendLine(m);
}
/* diff {d, e}: this console's song-select difficulty, on change */
static void ReportDiff() {
    if (s_myD < 0 || !s_connected) return;
    if (s_myD == s_sentD && s_myE == s_sentE) return;
    s_sentD = s_myD; s_sentE = s_myE;
    s_diffSentAt = (int64_t)NowMs();
    char m[64];
    snprintf(m, sizeof(m), "{\"t\":\"diff\",\"d\":%d,\"e\":%d}", s_myD, s_myE);
    SendLine(m);
}
/* the song picker's difficulty when the room forces it on everyone */
static bool ForcedDiff(int32_t* d, int32_t* e) {
    if (!s_sameDiff || s_amLeader) return false;
    for (int i = 0; i < s_memberCount; i++)
        if (strcmp(s_members[i].cid, s_leaderCid) == 0) {
            if (s_members[i].diff < 0) return false;
            *d = s_members[i].diff; *e = s_members[i].ed;
            return true;
        }
    return false;
}
static const char* DiffName(int32_t d, int32_t e) {
    static const char* DNM[] = {"EASY", "NORMAL", "HARD", "EXTREME"};
    if (d == 3 && e) return "EXTRA EXTREME";
    return DNM[d >= 0 && d < 4 ? d : 0];
}
static void SetScene(const char* w) {
    if (strcmp(s_scene, w) == 0) return;
    strncpy(s_scene, w, sizeof(s_scene) - 1);
    s_scene[sizeof(s_scene) - 1] = 0;
    if (s_code[0]) ReportScene();
}

static int64_t  s_countdownEndMs = 0;   /* >0 = a countdown is ticking */
static int64_t  s_countdownStartMs = 0; /* ms the countdown began */
static int64_t  s_goAt = 0;             /* ms the round's start arrived */
static int      s_notifyStyle = 0;      /* 0 avatars, 1 text, 2 off */

static Popup    s_popups[8];
static int      s_popupCount = 0;

const Popup* Popups(int* count) { *count = s_popupCount; return s_popups; }

void PushPopup(const char* kind, const char* cid, const char* name,
               int32_t value) {
    if (s_popupCount >= 8) {   /* push the oldest out */
        memmove(s_popups, s_popups + 1, sizeof(s_popups) - sizeof(Popup));
        s_popupCount--;
    }
    Popup& p = s_popups[s_popupCount++];
    strncpy(p.kind, kind, sizeof(p.kind) - 1); p.kind[sizeof(p.kind)-1]=0;
    strncpy(p.cid, cid, sizeof(p.cid) - 1);   p.cid[sizeof(p.cid)-1]=0;
    strncpy(p.name, name, sizeof(p.name) - 1); p.name[sizeof(p.name)-1]=0;
    p.value = value;
    p.bornAt = NowMs();
}

static void PushBanner(const char* fmt, ...);

/* route a PLAYER event to popups or a banner per notify_style */
static void RoutePlayerEvent(const char* k, const char* cid,
                             const char* nm, int32_t value) {
    bool onResults = s_resultsShownAt > 0;
    if (s_notifyStyle == 2) return;                        /* off */
    if (s_notifyStyle == 0) {                              /* avatars */
        if ((strcmp(k, "combo") == 0 || strcmp(k, "low_life") == 0 ||
             strcmp(k, "drop_out") == 0) && onResults) return;
        PushPopup(k, cid, nm, value);
        return;
    }
    /* text */
    if (strcmp(k, "combo") == 0) { if (!onResults) PushBanner("%s hit a big combo!", nm); }
    else if (strcmp(k, "low_life") == 0) { if (!onResults) PushBanner("%s is in danger!", nm); }
    else if (strcmp(k, "drop_out") == 0) { if (!onResults) PushBanner("%s dropped out!", nm); }
    else if (strcmp(k, "leader") == 0) PushBanner("%s is the leader", nm);
    else if (strcmp(k, "joined") == 0) PushBanner("%s joined", nm);
    else if (strcmp(k, "left") == 0) PushBanner("%s left", nm);
    else if (strcmp(k, "disconnected") == 0) PushBanner("%s disconnected", nm);
    else if (strcmp(k, "reconnected") == 0) PushBanner("%s reconnected", nm);
    else if (strcmp(k, "owner") == 0) PushBanner("%s is now the room owner", nm);
}

void PopupTest() {
    static const char* k[] = {
        "combo","low_life","drop_out","joined","left",
        "disconnected","reconnected","leader","owner"};
    for (auto& t : k)
        RoutePlayerEvent(t, s_myCid, s_myName,
                         strcmp(t, "combo") == 0 ? 100 : 0);
}

void SetRemoteAvatars(bool on) {
    if (Config::mpRemoteAvatars == on) return;
    Config::mpRemoteAvatars = on;
    Config::Save();
}

void SystemBanner(const char* text) {
    PushBanner("%s", text);
}

int NotifyStyle() { return s_notifyStyle; }
void SetNotifyStyle(int st) {
    s_notifyStyle = st;
    Config::mpNotifyStyle = st == 1 ? "text" : st == 2 ? "off" : "avatars";
    Config::Save();
}

static void PushBanner(const char* fmt, ...) {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (s_bannerCount >= 4) {
        memmove(s_banners, s_banners + 1, sizeof(Banner) * 3);
        s_bannerCount = 3;
    }
    Banner& b = s_banners[s_bannerCount++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b.text, sizeof(b.text), fmt, ap);
    va_end(ap);
    b.ttl = 3.0f;
    WriteLog("[MP-L] banner: %s\n", b.text);
}

/* the join didn't happen: keep why for the join screen, and say it in a
 * banner unless that screen is up to show it */
static void JoinFail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_joinErr, sizeof(s_joinErr), fmt, ap);
    va_end(ap);
    s_joinState = JOIN_FAILED;
    WriteLog("[MP-L] join '%s' failed: %s\n", s_joinCode, s_joinErr);
    if (!s_joinQuiet || !s_overlayOpen) PushBanner("%s", s_joinErr);
}

static void SetPhase(LPhase p, const char* status) {
    if (p != s_phase)
        WriteLog("[MP-L] phase %d -> %d\n", (int)s_phase, (int)p);
    s_phase = p;
    strncpy(s_statusLine, status, sizeof(s_statusLine) - 1);
    s_statusLine[sizeof(s_statusLine) - 1] = 0;
}

/* Overlay-close release guard: buttons (and a deflected stick) that were
 * held when the overlay closed stay blocked until released, so the game
 * never sees a "fresh" press from unblocking mid-hold. */
static uint64_t s_relGuard = 0;
static bool     s_relGuardStick = false;
static uint64_t s_baseMask = 0;
static bool     s_baseZeroLs = false;

static void PushBlocks() {
    /* the open overlay owns all input; ApplyBlocks() runs on every lobby
     * message and must not hand the game input back behind it */
    if (s_overlayOpen) {
        input::SetBlocked(~(uint64_t)nn::hid::Minus);
        input::SetZeroSticks(true);
        return;
    }
    input::SetBlocked(s_baseMask | s_relGuard);
    input::SetZeroSticks(s_baseZeroLs || s_relGuardStick);
}
static void SetBlocks(uint64_t mask, bool zeroLs) {
    s_baseMask = mask;
    s_baseZeroLs = zeroLs;
    PushBlocks();
}
static void GuardPump() {
    if (s_overlayOpen || (!s_relGuard && !s_relGuardStick)) return;
    s_relGuard &= input::NavHeld();
    if (s_relGuardStick) {
        int sx = 0, sy = 0;
        input::NavStick(&sx, &sy);
        if (sx < 8000 && sx > -8000 && sy < 8000 && sy > -8000)
            s_relGuardStick = false;
    }
    PushBlocks();
}
/* drops queued taps and every block, except the open overlay's full
 * block (leaving a room from the overlay must not hand the game input) */
static void ClearAllInput() { input::ResetAll(); s_relGuard = 0;
                              s_relGuardStick = false; s_baseMask = 0;
                              s_baseZeroLs = false; PushBlocks(); }

static const uint64_t MASK_FOLLOWER =
    nn::hid::A | nn::hid::Up | nn::hid::Down | nn::hid::L | nn::hid::R |
    nn::hid::Y;
/* same_diff rooms: the difficulty follows the picker too */
static const uint64_t MASK_FOLLOWER_SD =
    MASK_FOLLOWER | nn::hid::Left | nn::hid::Right;
static bool IsFollowerMask(uint64_t m) {
    return m == MASK_FOLLOWER || m == MASK_FOLLOWER_SD;
}
static const uint64_t MASK_ALL_NO_MINUS = ~(uint64_t)nn::hid::Minus;

/* rooms with 2+ members: Retry is impossible and pause is group-synced */
static bool Guarded() { return s_code[0] && s_memberCount >= 2; }

static const char* MenuPageName(int p) {
    switch (p) {
    case mp::menu::P_RES_SCORE:  return "res_score";
    case mp::menu::P_RES_VP:     return "res_vp";
    case mp::menu::P_RES_MENU:   return "res_menu";
    case mp::menu::P_RES_EXIT:   return "res_exit";
    case mp::menu::P_PAUSE:      return "pause";
    case mp::menu::P_PAUSE_QUIT: return "pause_quit";
    default:                     return "none";
    }
}

/* telemetry: page/cursor transitions, swallowed-input events and every
 * driver/fallback/recovery step -> server MSG line + hook_log */
static void DbgMenu(int page, int cursor, const char* ev) {
    char m[160];
    snprintf(m, sizeof(m),
        "{\"t\":\"dbg_menu\",\"page\":\"%s\",\"cur\":%d,\"ev\":\"%s\"}",
        MenuPageName(page), cursor, ev ? ev : "");
    SendLine(m);
    WriteLog("[MP-L] menu %s cur=%d ev=%s\n",
             MenuPageName(page), cursor, ev ? ev : "");
}

/* ============================== inbound ============================ */

void PushLine(const char* line, int len) {
    if (!line || len <= 0) return;
    if (len > (int)sizeof(s_inQ[0].data) - 1)
        len = (int)sizeof(s_inQ[0].data) - 1;
    std::lock_guard<std::mutex> lk(s_inQMtx);
    if (s_inCount == 32) {
        s_inHead = (s_inHead + 1) & 31;
        s_inCount--;
    }
    uint32_t idx = (s_inHead + s_inCount) & 31;
    memcpy(s_inQ[idx].data, line, (size_t)len);
    s_inQ[idx].data[len] = 0;
    s_inQ[idx].len = (uint16_t)len;
    s_inCount++;
}

static void ResetLobbyState() {
    s_phase = L_NONE;
    s_code[0] = 0; s_leaderCid[0] = 0;
    s_webKey[0] = 0;
    s_memberCount = 0; s_boardCount = 0; s_standingCount = 0;
    s_selPv = -1; s_leaderPv = -1; s_localPv = -1;
    s_folLeaderPv = -1; s_folTarget = -1; s_folMissing = false;
    s_folFail = 0; s_folGaveUp = false; s_folTD = -1; s_folTE = -1;
    s_committed = false; s_amPlayer = false;
    s_loadedSent = false; s_startSeen = false;
    s_apActive = false; s_statePending = false;
    s_launchStage = 0; s_jumpVerify = 0;
    s_jumpFailedDiff = false;
    s_launchBlocking = false; s_amLeader = false;
    s_resumePending = false;
    s_standingsComplete = false; s_backInSelSent = false;
    s_resultInitAt = 0;
    s_resultsShownAt = 0;
    s_playedRound = 0; s_finalSentRound = 0;
    s_finalLine[0] = 0; s_finalLineRound = 0;
    s_sameDiff = false;
    s_noFail = -1; s_roundNoFail = -1;
    s_resPanel = 0; s_resPage = 0; s_resPanelAt = 0;
    s_folWantD = -1; s_folWantE = 0;
    s_folDiffBanner[0] = s_folDiffBanner[1] = s_folDiffBanner[2] = -1;
    s_diffSyncTries = 0; s_diffSyncSig = -1;
    s_roundOver = false; s_autoReturn = false;
    s_standingsRound = 0;
    s_launchHandledRound = -1;   /* round numbers restart in every room */
    s_notReady[0] = 0;
    s_unavailFor[0] = 0;
    s_ready = true; s_readyReason[0] = 0;
    s_statusLine[0] = 0;
    s_quitting = false; s_prevPvPaused = 0;
    s_topRightBusy = false;
    s_retryWatch = false; s_retryRecStage = 0;
    s_retBannered = false; s_scoreTries = 0;
    menu::End();
}

static void HandleLobby(const char* line, jsmntok_t* tok, int ntok) {
    auto gs = [&](const char* k, char* out, int cap) {
        out[0] = 0;
        int i = mpjson::Find(line, tok, ntok, k);
        if (i >= 0) mpjson::Copy(line, tok[i], out, cap);
    };
    auto gi = [&](const char* k, int32_t* v) {
        int i = mpjson::Find(line, tok, ntok, k), x;
        if (i >= 0 && mpjson::ToInt(line, tok[i], &x)) { *v = x; return true; }
        return false;
    };

    gs("code", s_code, sizeof(s_code));
    gs("phase", s_serverPhase, sizeof(s_serverPhase));
    gs("leader", s_leaderCid, sizeof(s_leaderCid));
    gs("owner", s_ownerCid, sizeof(s_ownerCid));
    gs("pick_mode", s_pickMode, sizeof(s_pickMode));
    gs("assigned", s_assignedCid, sizeof(s_assignedCid));
    if (strcmp(s_ownerCid, s_myCid) != 0) s_webKey[0] = 0;   /* the owner's only */
    {   int gi2 = mpjson::Find(line, tok, ntok, "group_paused");
        if (gi2 >= 0) s_groupPaused = line[tok[gi2].start] == 't'; }
    {   int sd = mpjson::Find(line, tok, ntok, "same_diff");
        s_sameDiff = sd >= 0 && line[tok[sd].start] == 't'; }
    {   int nf = mpjson::Find(line, tok, ntok, "no_fail");
        s_noFail = nf < 0 ? -1 : (line[tok[nf].start] == 't' ? 1 : 0);
        if (strcmp(s_serverPhase, "SELECT") == 0) s_roundNoFail = -1; }
    int32_t rd = 0; gi("round", &rd); s_round = rd;
    int32_t spv = -1; if (gi("sel_pv", &spv)) s_selPv = spv;
    bool wasLeader = s_amLeader;
    s_amLeader = strcmp(s_leaderCid, s_myCid) == 0;
    if (s_amLeader && !wasLeader) {
        /* follower -> leader: drop the latched follow target */
        s_localPv = s_leaderPv = -1;
        s_folLeaderPv = -1; s_folTarget = -1; s_folMissing = false;
        s_folFail = 0; s_folGaveUp = false; s_folTD = -1; s_folTE = -1;
        s_jumpFailedDiff = false;
        s_ackSig[0] = 0;
    }

    s_memberCount = 0;
    int mi = mpjson::Find(line, tok, ntok, "members");
    if (mi >= 0 && tok[mi].type == JSMN_ARRAY) {
        int i = mi + 1;
        while (i < ntok && s_memberCount < 8 &&
               tok[i].type == JSMN_OBJECT) {
            int end = i + mpjson::Skip(tok, i, ntok);
            MRec& m = s_members[s_memberCount];
            memset(&m, 0, sizeof(m));
            int j;
            if ((j = mpjson::Find(line, tok + i, end - i, "cid")) >= 0)
                mpjson::Copy(line, tok[i + j], m.cid, sizeof(m.cid));
            if ((j = mpjson::Find(line, tok + i, end - i, "name")) >= 0)
                mpjson::Copy(line, tok[i + j], m.name, sizeof(m.name));
            if ((j = mpjson::Find(line, tok + i, end - i, "connected")) >= 0)
                m.connected = line[tok[i + j].start] == 't';
            if ((j = mpjson::Find(line, tok + i, end - i, "role")) >= 0)
                mpjson::Copy(line, tok[i + j], m.role, sizeof(m.role));
            if ((j = mpjson::Find(line, tok + i, end - i, "loaded")) >= 0)
                m.loaded = line[tok[i + j].start] == 't';
            if ((j = mpjson::Find(line, tok + i, end - i, "ready")) >= 0)
                m.ready = line[tok[i + j].start] == 't';
            if ((j = mpjson::Find(line, tok + i, end - i, "reason")) >= 0)
                mpjson::Copy(line, tok[i + j], m.reason, sizeof(m.reason));
            int v = -1;
            if ((j = mpjson::Find(line, tok + i, end - i, "module")) >= 0)
                mpjson::ToInt(line, tok[i + j], &v);
            m.av.module = v;
            v = -1;
            if ((j = mpjson::Find(line, tok + i, end - i, "chara")) >= 0)
                mpjson::ToInt(line, tok[i + j], &v);
            m.av.chara = v;
            /* mh is a u32: parse as text so values >= 2^31 survive */
            if ((j = mpjson::Find(line, tok + i, end - i, "mh")) >= 0) {
                char hb[16] = "";
                mpjson::Copy(line, tok[i + j], hb, sizeof(hb));
                m.av.mh = (uint32_t)strtoul(hb, nullptr, 10);
            }
            if ((j = mpjson::Find(line, tok + i, end - i, "img")) >= 0)
                mpjson::Copy(line, tok[i + j], m.av.img, sizeof(m.av.img));
            if ((j = mpjson::Find(line, tok + i, end - i, "sr")) >= 0 &&
                tok[i + j].type == JSMN_ARRAY) {
                int a = i + j + 1;
                for (int k = 0; k < tok[i + j].size && k < 5 && a < end;
                     k++, a += mpjson::Skip(tok, a, ntok)) {
                    int sv = 0;
                    if (mpjson::ToInt(line, tok[a], &sv)) m.sr[k] = sv;
                }
            }
            m.plat = PLAT_UNKNOWN; m.diff = -1; m.ed = 0;
            if ((j = mpjson::Find(line, tok + i, end - i, "plat")) >= 0) {
                char pb[8] = "";
                mpjson::Copy(line, tok[i + j], pb, sizeof(pb));
                m.plat = strcmp(pb, "pc") == 0 ? PLAT_PC
                       : strcmp(pb, "switch") == 0 ? PLAT_SWITCH : PLAT_UNKNOWN;
            }
            v = -1;
            if ((j = mpjson::Find(line, tok + i, end - i, "diff")) >= 0 &&
                mpjson::ToInt(line, tok[i + j], &v) && v >= 0 && v <= 3)
                m.diff = (int8_t)v;
            v = 0;
            if ((j = mpjson::Find(line, tok + i, end - i, "ed")) >= 0 &&
                mpjson::ToInt(line, tok[i + j], &v))
                m.ed = (m.diff == 3 && v == 1) ? 1 : 0;
            if ((j = mpjson::Find(line, tok + i, end - i, "nosong")) >= 0)
                m.nosong = line[tok[i + j].start] == 't';
            s_memberCount++;
            i = end;
        }
    }
}

static void HandleBoard(const char* line, jsmntok_t* tok, int ntok) {
    int mi = mpjson::Find(line, tok, ntok, "e");
    if (mi < 0 || tok[mi].type != JSMN_ARRAY) return;
    s_boardCount = 0;
    int i = mi + 1;
    while (i < ntok && s_boardCount < 8 && tok[i].type == JSMN_OBJECT) {
        int end = i + mpjson::Skip(tok, i, ntok), j;
        BoardRow& b = s_board[s_boardCount];
        memset(&b, 0, sizeof(b)); b.connected = true;
        if ((j = mpjson::Find(line, tok + i, end - i, "cid")) >= 0)
            mpjson::Copy(line, tok[i + j], b.cid, sizeof(b.cid));
        if ((j = mpjson::Find(line, tok + i, end - i, "name")) >= 0)
            mpjson::Copy(line, tok[i + j], b.name, sizeof(b.name));
        int v;
        if ((j = mpjson::Find(line, tok + i, end - i, "score")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v))
            b.score = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "pct")) >= 0)
            b.pct = mpjson::ToNum(line, tok[i + j], 0.0f, 200.0f);
        if ((j = mpjson::Find(line, tok + i, end - i, "life")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v))
            b.life = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "rank")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v))
            b.rank = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "connected")) >= 0)
            b.connected = line[tok[i + j].start] == 't';
        s_boardCount++;
        i = end;
    }
}

static void HandleStandings(const char* line, jsmntok_t* tok, int ntok) {
    int mi = mpjson::Find(line, tok, ntok, "e");
    if (mi < 0 || tok[mi].type != JSMN_ARRAY) return;
    {   int32_t v; int ri = mpjson::Find(line, tok, ntok, "round");
        if (ri >= 0 && mpjson::ToInt(line, tok[ri], &v)) s_standingsRound = v; }
    s_standingCount = 0;
    int i = mi + 1;
    while (i < ntok && s_standingCount < 8 && tok[i].type == JSMN_OBJECT) {
        int end = i + mpjson::Skip(tok, i, ntok), j;
        StandingRow& r = s_standings[s_standingCount];
        memset(&r, 0, sizeof(r));
        r.connected = true;
        if ((j = mpjson::Find(line, tok + i, end - i, "cid")) >= 0)
            mpjson::Copy(line, tok[i + j], r.cid, sizeof(r.cid));
        if ((j = mpjson::Find(line, tok + i, end - i, "connected")) >= 0)
            r.connected = line[tok[i + j].start] == 't';
        if ((j = mpjson::Find(line, tok + i, end - i, "name")) >= 0)
            mpjson::Copy(line, tok[i + j], r.name, sizeof(r.name));
        int v;
        if ((j = mpjson::Find(line, tok + i, end - i, "score")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v)) r.score = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "pct")) >= 0)
            r.pct = mpjson::ToNum(line, tok[i + j], 0.0f, 200.0f);
        if ((j = mpjson::Find(line, tok + i, end - i, "max_combo")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v)) r.max_combo = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "life")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v)) r.life = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "rank")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v)) r.rank = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "final")) >= 0)
            r.final = line[tok[i + j].start] == 't';
        r.diff = -1; r.ed = 0;
        if ((j = mpjson::Find(line, tok + i, end - i, "diff")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v) && v >= 0 && v <= 3)
            r.diff = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "ed")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v))
            r.ed = (r.diff == 3 && v == 1) ? 1 : 0;
        if ((j = mpjson::Find(line, tok + i, end - i, "jc")) >= 0 &&
            tok[i + j].type == JSMN_ARRAY && tok[i + j].size >= 5) {
            int a = i + j + 1;
            for (int q = 0; q < 5 && a < end; q++, a += mpjson::Skip(tok, a, ntok)) {
                int jv = 0;
                if (mpjson::ToInt(line, tok[a], &jv) && jv > 0) r.jc[q] = jv;
            }
            r.hasJc = true;
        }
        if ((j = mpjson::Find(line, tok + i, end - i, "border")) >= 0)
            r.border = mpjson::ToNum(line, tok[i + j], 0.0f, 200.0f);
        r.clear = -1;
        if ((j = mpjson::Find(line, tok + i, end - i, "clear")) >= 0) {
            char c0 = line[tok[i + j].start];
            r.clear = c0 == 't' ? 1 : (c0 == 'f' ? 0 : -1);
        }
        if ((j = mpjson::Find(line, tok + i, end - i, "drop")) >= 0)
            r.drop = line[tok[i + j].start] == 't';
        if ((j = mpjson::Find(line, tok + i, end - i, "autoplay")) >= 0)
            r.autoplay = line[tok[i + j].start] == 't';
        s_standingCount++;
        i = end;
    }
    int ci = mpjson::Find(line, tok, ntok, "complete");
    if (ci >= 0 && line[tok[ci].start] == 't')
        s_standingsComplete = true;
}

/* True if cid appears in the JSON array of strings at tok[arr]. */
static bool CidInArray(const char* line, const jsmntok_t* tok, int ntok,
                       int arr, const char* cid) {
    for (int k = 0, j = arr + 1; k < tok[arr].size && j < ntok;
         k++, j += mpjson::Skip(tok, j, ntok)) {
        char c[24] = "";
        mpjson::Copy(line, tok[j], c, sizeof(c));
        if (strcmp(c, cid) == 0) return true;
    }
    return false;
}

static void ProcessLine(const char* line) {
    jsmn_parser ps; jsmn_init(&ps);
    jsmntok_t tok[384];      /* 8 members x ~20 tokens + header */
    int ntok = jsmn_parse(&ps, line, strlen(line), tok, 384);
    if (ntok < 1 || tok[0].type != JSMN_OBJECT) return;
    int ti = mpjson::Find(line, tok, ntok, "t");
    char t[24] = "";
    if (ti >= 0) mpjson::Copy(line, tok[ti], t, sizeof(t));

    std::lock_guard<std::recursive_mutex> lk(s_mtx);

    if (strcmp(t, "welcome") == 0) {
        {   int ci = mpjson::Find(line, tok, ntok, "cid");
            char c[sizeof(s_myCid)] = "";
            if (ci >= 0 && tok[ci].type == JSMN_STRING)
                mpjson::Copy(line, tok[ci], c, sizeof(c));
            if (c[0]) memcpy(s_myCid, c, sizeof(s_myCid));
        }
        s_connected = true;
        s_offlineSince = 0;
        if (s_leaveOnReconnect) {
            /* we left the room while offline: tell the server, and ignore
             * the room resync it sends with the welcome */
            s_leaveOnReconnect = false;
            s_ignoreRoomUntil = NowMs() + 4000;
            SendLine("{\"t\":\"lobby_leave\"}");
        }
        if (s_code[0]) { s_expectLobby = true; s_expectLobbyAt = NowMs() + 3000; }
        /* still waiting on this round's results: resend our final (the
         * server ignores it once the round is over, and a repeat only
         * overwrites the same values) */
        if (s_code[0] && s_phase == L_RESULTS && s_finalLine[0] &&
            s_finalLineRound == s_playedRound) {
            WriteLog("[MP-L] reconnect: resending final r%d\n", s_finalLineRound);
            SendLine(s_finalLine);
        }
        if (s_inLobbyDirty()) s_readyDirty = true;
        s_sentD = -2;                 /* a new session: report it again */
        ReportDiff();
        ReportAutoPlay(AutoPlay::g_enabled);
        ReportScene();
        avatar::OnWelcome();
        return;
    }
    if (s_ignoreRoomUntil) {
        if (strcmp(t, "lobby_left") == 0 || NowMs() >= s_ignoreRoomUntil) {
            s_ignoreRoomUntil = 0;
            if (strcmp(t, "lobby_left") == 0) return;
        } else if (strcmp(t, "lobby_error") == 0) {
            s_ignoreRoomUntil = 0;     /* server had already dropped us */
            return;
        } else {
            return;                    /* stale room traffic */
        }
    }
    if (strcmp(t, "lobby") == 0) {
        HandleLobby(line, tok, ntok);
        s_expectLobby = false;
        s_joinState = JOIN_IDLE;
        if (s_phase == L_NONE || s_phase == L_DROPPED) {
            ReportAutoPlay(AutoPlay::g_enabled);
            ReportScene();
            SetPhase(strcmp(s_serverPhase, "SELECT") == 0
                     ? L_SELECT : L_SPECTATING,
                     s_amLeader ? "You're the leader - pick a song"
                                : "In lobby - waiting for the leader");
        }
        ApplyBlocks();
        return;
    }
    if (strcmp(t, "web_key") == 0) {
        char code[8] = "";
        int i = mpjson::Find(line, tok, ntok, "code");
        if (i >= 0) mpjson::Copy(line, tok[i], code, sizeof(code));
        if (!s_code[0] || strcmp(code, s_code) != 0) return;   /* not this room's */
        s_webKey[0] = s_webUrl[0] = 0;
        if ((i = mpjson::Find(line, tok, ntok, "key")) >= 0 && tok[i].type == JSMN_STRING)
            mpjson::Copy(line, tok[i], s_webKey, sizeof(s_webKey));
        if ((i = mpjson::Find(line, tok, ntok, "url")) >= 0 && tok[i].type == JSMN_STRING)
            mpjson::Copy(line, tok[i], s_webUrl, sizeof(s_webUrl));
        return;
    }
    if (strcmp(t, "lobby_left") == 0) {
        char r[16] = ""; int i = mpjson::Find(line, tok, ntok, "reason");
        if (i >= 0) mpjson::Copy(line, tok[i], r, sizeof(r));
        ResetLobbyState();
        ClearAllInput();
        PushBanner(strcmp(r, "kicked") == 0
               ? "You've been removed from the lobby" : "Left the lobby");
        SetPhase(L_NONE, "");
        return;
    }
    if (strcmp(t, "lobby_error") == 0) {
        char r[24] = ""; int i = mpjson::Find(line, tok, ntok, "code");
        if (i >= 0) mpjson::Copy(line, tok[i], r, sizeof(r));
        /* the answer to our join */
        if (s_joinState == JOIN_PENDING) {
            if (strcmp(r, "not_found") == 0) {
                JoinFail("Room %s not found - check the code", s_joinCode);
                return;
            }
            if (strcmp(r, "full") == 0) {
                JoinFail("Room %s is full", s_joinCode);
                return;
            }
            if (strcmp(r, "already_in_lobby") == 0) {
                JoinFail("You're already in a room");
                return;
            }
            if (strcmp(r, "rate_limited") == 0) {
                JoinFail("Too many tries - wait a minute");
                return;
            }
        }
        /* the server's request budgets (room creation, join attempts) */
        if (strcmp(r, "rate_limited") == 0) {
            PushBanner("Too many requests - wait a minute");
            return;
        }
        if (strcmp(r, "server_full") == 0) {
            PushBanner("The server has no room left for another room");
            return;
        }
        PushBanner("Lobby error: %s", r);
        return;
    }
    if (strcmp(t, "sel") == 0) {
        int32_t pv = -1, i = mpjson::Find(line, tok, ntok, "pv"), v;
        if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) pv = v;
        s_selPv = pv;
        int ni = mpjson::Find(line, tok, ntok, "name");
        s_selName[0] = 0;
        if (ni >= 0) mpjson::Copy(line, tok[ni], s_selName, sizeof(s_selName));
        if (!s_amLeader) {
            s_leaderPv = pv;
            s_localPv = pv;
            s_jumpFailedDiff = false;
            s_ackSig[0] = 0;
        }
        return;
    }
    if (strcmp(t, "sel_status") == 0) {
        s_notReady[0] = 0;
        char unavail[96]; unavail[0] = 0;
        int off = 0, uoff = 0;
        const char* keys[] = {"missing", "not_ready", "cant"};
        for (int k = 0; k < 3; k++) {
            int mi = mpjson::Find(line, tok, ntok, keys[k]);
            if (mi < 0 || tok[mi].type != JSMN_ARRAY) continue;
            int i = mi + 1;
            while (i < ntok && tok[i].type == JSMN_OBJECT) {
                int end = i + mpjson::Skip(tok, i, ntok), j;
                char nm[40] = "", rs[24] = "";
                if ((j = mpjson::Find(line, tok + i, end - i, "name")) >= 0)
                    mpjson::Copy(line, tok[i + j], nm, sizeof(nm));
                if (k == 1 &&
                    (j = mpjson::Find(line, tok + i, end - i, "reason")) >= 0)
                    mpjson::Copy(line, tok[i + j], rs, sizeof(rs));
                /* players without the song are flagged per member
                 * (nosong), not listed here */
                if (k != 0 && strcmp(rs, "wrong_song") != 0)
                    mpjson::Appendf(s_notReady, sizeof(s_notReady), &off,
                                    "%s%s%s; ", nm, rs[0] ? ":" : "", rs);
                /* missing songs have their own tag (members' nosong);
                 * this list is who has it but can't select it */
                if (k == 2 && nm[0] && uoff < (int)sizeof(unavail) - 20)
                    mpjson::Appendf(unavail, sizeof(unavail), &uoff,
                                    "%s, ", nm);
                i = end;
            }
        }
        if (uoff >= 2) unavail[uoff - 2] = 0;   /* trim ", " */
        if (strcmp(unavail, s_unavailFor) != 0) {
            strncpy(s_unavailFor, unavail, sizeof(s_unavailFor) - 1);
            s_unavailFor[sizeof(s_unavailFor) - 1] = 0;
            if (s_amLeader && unavail[0])
                PushBanner("%s can't select this song", unavail);
        }
        return;
    }
    if (strcmp(t, "launch") == 0) {
        int32_t rd = 0, i = mpjson::Find(line, tok, ntok, "round"), v;
        if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) rd = v;
        if (s_launchHandledRound == rd) return;
        s_launchHandledRound = rd;
        s_round = rd;
        s_roundOver = false;
        {   int nf = mpjson::Find(line, tok, ntok, "no_fail");
            s_roundNoFail = nf < 0 ? -1 : (line[tok[nf].start] == 't' ? 1 : 0); }
        int32_t pv = 0; i = mpjson::Find(line, tok, ntok, "pv");
        if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) pv = v;
        s_selPv = pv;
        s_boardCount = 0;
        s_lastSentScore = -1;
        if (s_resultsShownAt == 0)
            s_standingCount = 0;   /* don't show last round's standings */
        if (s_committed) {
            s_amPlayer = true;
            SetPhase(L_LOADING, "Loading...");
            SetBlocks(MASK_ALL_NO_MINUS, false);
        } else if (s_resultsShownAt > 0 ||
                   (!s_selActive && !CustomizeSelUi::IsInCustomizeScene())) {
            /* away from song select: can't join this round */
            s_amPlayer = false;
            SetPhase(L_SPECTATING, "Spectating this round");
            char m[64];
            snprintf(m, sizeof(m), "{\"t\":\"launch_fail\",\"round\":%d}", rd);
            SendLine(m);
            PushBanner("Round started while you were away - spectating");
            if (s_resultsShownAt > 0) s_autoReturn = true;
        } else {
            bool player = true;
            int pi = mpjson::Find(line, tok, ntok, "players");
            if (pi >= 0 && tok[pi].type == JSMN_ARRAY)
                player = CidInArray(line, tok, ntok, pi, s_myCid);
            WriteLog("[MP-L] launch r%d players idx=%d type=%d size=%d me='%s' -> %d\n",
                     rd, pi, pi >= 0 ? (int)tok[pi].type : -1,
                     pi >= 0 ? tok[pi].size : -1, s_myCid, (int)player);
            s_amPlayer = player;
            if (player) {
                s_leaderPv = pv;
                s_localPv = pv;
                SetPhase(L_LAUNCHING, "Leader started - finish module "
                                      "selection to join");
                s_launchStage = 0;
                s_launchDiffTries = 0;
                s_launchBlocking = false;
                s_launchStartMs = NowMs();
                ClearAllInput();
                PushBanner("Leader started the game!");
            } else {
                SetPhase(L_SPECTATING, "Spectating this round");
                ClearAllInput();
                PushBanner("Spectating: %s", s_selName);
            }
        }
        return;
    }
    if (strcmp(t, "countdown") == 0) {
        int i = mpjson::Find(line, tok, ntok, "secs");
        double secs = 0;
        if (i >= 0) secs = mpjson::ToNum(line, tok[i], 0.0f, 60.0f);
        if (secs > 0) {
            s_countdownEndMs = NowMs() + (int64_t)(secs * 1000);
            s_countdownStartMs = NowMs();
        }
        return;
    }
    if (strcmp(t, "start") == 0) {
        int32_t rd = 0, i = mpjson::Find(line, tok, ntok, "round"), v;
        if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) rd = v;
        if (s_startSeen && rd == s_round) return;
        s_startSeen = true;
        s_round = rd;
        s_countdownEndMs = 0;
        s_goAt = NowMs();
        if (!s_amPlayer) {
            SetPhase(L_SPECTATING, "Spectating this round");
            return;
        }
        bool dropped = false;
        int di = mpjson::Find(line, tok, ntok, "dropped");
        if (di >= 0 && tok[di].type == JSMN_ARRAY)
            dropped = CidInArray(line, tok, ntok, di, s_myCid);
        s_resumePending = true;
        s_resumeTries = 0;
        s_resumeLastMs = 0;
        s_playedRound = rd;
        s_lastSentScore = -1;
        s_statePending = true;
        if (dropped) {
            SetPhase(L_DROPPED, "Dropped - playing solo");
            ClearAllInput();
            PushBanner("Dropped from this round - playing solo");
        } else {
            SetPhase(L_PLAYING, "");
            SetBlocks(nn::hid::Plus, false);
        }
        return;
    }
    if (strcmp(t, "pause") == 0 || strcmp(t, "resume") == 0) {
        bool p = (t[0] == 'p');
        s_groupPaused = p;
        char by[24] = "";
        int i = mpjson::Find(line, tok, ntok, "by");
        if (i >= 0) mpjson::Copy(line, tok[i], by, sizeof(by));
        char nm[40] = "";
        for (int q = 0; q < s_memberCount; q++)
            if (strcmp(s_members[q].cid, by) == 0) {
                strncpy(nm, s_members[q].name, sizeof(nm) - 1); break; }
        bool sync = false;
        {   int si = mpjson::Find(line, tok, ntok, "sync");
            if (si >= 0 && line[tok[si].start] == 't') sync = true; }
        if (!sync)
            PushBanner("%s by %s", p ? "Paused" : "Resumed",
                       nm[0] ? nm : by);
        /* state only: the OnPvGameFrame reconciler taps Plus to make the
         * local game match the group state */
        ApplyBlocks();
        return;
    }
    if (strcmp(t, "pause_vote") == 0) {
        int i;
        int k = mpjson::Find(line, tok, ntok, "kind");
        if (k >= 0) mpjson::Copy(line, tok[k], s_voteKind, sizeof(s_voteKind));
        i = mpjson::Find(line, tok, ntok, "need");
        int v; if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) s_voteNeed = v;
        s_voteEndsIn = 0;
        i = mpjson::Find(line, tok, ntok, "ends_in");
        if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) s_voteEndsIn = v;
        s_voteExpired = false;
        i = mpjson::Find(line, tok, ntok, "expired");
        if (i >= 0 && line[tok[i].start] == 't') s_voteExpired = true;
        int ai = mpjson::Find(line, tok, ntok, "agreed");
        s_voteAgreed = 0;
        if (ai >= 0 && tok[ai].type == JSMN_ARRAY)
            s_voteAgreed = tok[ai].size;
        if (s_voteExpired) {
            s_voteKind[0] = 0;
            PushBanner("%s vote expired", "Pause/resume");
        } else {
            PushBanner("%s vote: %d/%d agreed - press + to agree (%ds)",
                       strcmp(s_voteKind, "pause") == 0 ? "Pause" : "Resume",
                       s_voteAgreed, s_voteNeed, s_voteEndsIn);
        }
        return;
    }
    if (strcmp(t, "board") == 0) { HandleBoard(line, tok, ntok); return; }
    if (strcmp(t, "standings") == 0) {
        HandleStandings(line, tok, ntok);
        return;
    }
    if (strcmp(t, "round_end") == 0) {
        int i = mpjson::Find(line, tok, ntok, "next_leader");
        char nl[24] = "";
        if (i >= 0) mpjson::Copy(line, tok[i], nl, sizeof(nl));
        s_selPv = -1; s_leaderPv = -1; s_localPv = -1;
    s_folLeaderPv = -1; s_folTarget = -1; s_folMissing = false;
    s_folFail = 0; s_folGaveUp = false; s_folTD = -1; s_folTE = -1;
        s_committed = false; s_startSeen = false; s_loadedSent = false;
        s_jumpFailedDiff = false; s_standingsComplete = false;
        s_backInSelSent = false; s_resultInitAt = 0;
        s_unavailFor[0] = 0;
        s_amLeader = strcmp(nl, s_myCid) == 0;
        s_roundOver = true;
        s_groupPaused = false;
        if (s_selActive) {
            SetPhase(L_SELECT, s_amLeader ? "You're the leader - pick a song"
                                          : "Back in lobby - next round");
        } else {
            /* on results / in the song / loading: never become an
               L_SELECT follower away from song select */
            SetPhase(L_RESULTS, "Round over");
            s_autoReturn = true;
        }
        ApplyBlocks();
        if (s_amLeader) PushBanner("You're the leader - pick a song");
        else PushBanner("Round over - back in song select");
        return;
    }
    if (strcmp(t, "event") == 0) {
        char k[24] = "", nm[40] = "", cid[20] = "";
        int i = mpjson::Find(line, tok, ntok, "kind");
        if (i >= 0) mpjson::Copy(line, tok[i], k, sizeof(k));
        i = mpjson::Find(line, tok, ntok, "name");
        if (i >= 0) mpjson::Copy(line, tok[i], nm, sizeof(nm));
        i = mpjson::Find(line, tok, ntok, "cid");
        if (i >= 0) mpjson::Copy(line, tok[i], cid, sizeof(cid));
        int32_t value = 0;
        i = mpjson::Find(line, tok, ntok, "value");
        if (i >= 0) mpjson::ToInt(line, tok[i], &value);
        if (strcmp(k, "no_fail") == 0) {
            PushBanner(value ? "No Fail Mode on: songs don't end when the "
                               "life gauge runs out"
                             : "No Fail Mode off: a song ends when the life "
                               "gauge runs out");
            return;
        }
        if (strcmp(k, "same_diff") == 0) {
            PushBanner(value ? "Same difficulty: everyone plays the song "
                               "picker's difficulty"
                             : "Same difficulty off: pick your own difficulty");
            return;
        }
        /* the results screen has its own standings; don't cover its title */
        bool onResults = s_resultsShownAt > 0;
        bool player = strcmp(k, "combo") == 0 || strcmp(k, "low_life") == 0 ||
            strcmp(k, "drop_out") == 0 ||
            strcmp(k, "joined") == 0 || strcmp(k, "left") == 0 ||
            strcmp(k, "disconnected") == 0 || strcmp(k, "reconnected") == 0 ||
            strcmp(k, "leader") == 0 || strcmp(k, "owner") == 0;
        if (player) { RoutePlayerEvent(k, cid, nm, value); return; }
        if (strcmp(k, "combo") == 0) { if (!onResults) PushBanner("%s hit a big combo!", nm); }
        else if (strcmp(k, "low_life") == 0) { if (!onResults) PushBanner("%s is in danger!", nm); }
        else if (strcmp(k, "leader") == 0) PushBanner("%s is the leader", nm);
        else if (strcmp(k, "joined") == 0) PushBanner("%s joined", nm);
        else if (strcmp(k, "left") == 0) PushBanner("%s left", nm);
        else if (strcmp(k, "disconnected") == 0) PushBanner("%s disconnected", nm);
        else if (strcmp(k, "reconnected") == 0) PushBanner("%s reconnected", nm);
        else if (strcmp(k, "owner") == 0) PushBanner("%s is now the room owner", nm);
        return;
    }
}

static bool s_inLobbyDirty() {
    return s_phase == L_SELECT || s_phase == L_LAUNCHING ||
           s_phase == L_LOADING || s_phase == L_PLAYING ||
           s_phase == L_SPECTATING || s_phase == L_RESULTS;
}

static void ApplyBlocks() {
    /* follower lock only on the song list itself: the module picker /
     * customize run inside song select and must stay usable */
    if (s_phase == L_SELECT && !s_amLeader && s_selActive && s_selBrowsing) {
        /* Left/Right only lock while there is a difficulty to follow (a
         * picker on an older build reports none) */
        int32_t fd = -1, fe = 0;
        SetBlocks(ForcedDiff(&fd, &fe) ? MASK_FOLLOWER_SD : MASK_FOLLOWER, true);
    }
    else if (s_phase == L_LOADING ||
             (s_phase == L_LAUNCHING && s_launchBlocking))
        SetBlocks(MASK_ALL_NO_MINUS, false);
    else if (s_resultsShownAt > 0 && s_autoReturn)
        /* the blind driver owns results once the round is over */
        SetBlocks(MASK_ALL_NO_MINUS, false);
    else if (s_resultsShownAt > 0 && s_resPanel == 2)
        SetBlocks(MASK_ALL_NO_MINUS, true);      /* our panel is up */
    else if (s_resultsShownAt > 0 && s_resPanel == 1)
        SetBlocks(nn::hid::A | nn::hid::Y, false);   /* the next A is ours */
    else if (s_resultsShownAt > 0 && s_resPanel == 3)
        SetBlocks(nn::hid::Y, false);
    else if (s_phase == L_PLAYING && Guarded())
        /* real Plus never reaches the game in a guarded room: it becomes
         * a group pause/resume request instead; the pause-menu model
         * gates everything else */
        SetBlocks(nn::hid::Plus, false);
    else
        SetBlocks(0, false);
}

/* leave the room on this side only (offline): the server is told on the
 * next welcome, and its room resync is ignored until it confirms */
static void LocalLeave(const char* why) {
    WriteLog("[MP-L] local leave: %s\n", why);
    ResetLobbyState();
    ClearAllInput();
    SetPhase(L_NONE, "");
    ApplyBlocks();
    s_leaveOnReconnect = true;
    PushBanner("%s", why);
}

/* ============================ per-frame ============================ */

void Pump() {
    for (;;) {
        char line[4096];
        {
            std::lock_guard<std::mutex> g(s_inQMtx);
            if (s_inCount == 0) break;
            uint16_t n = s_inQ[s_inHead].len;
            memcpy(line, s_inQ[s_inHead].data, n);
            line[n] = 0;
            s_inHead = (s_inHead + 1) & 31;
            s_inCount--;
        }
        ProcessLine(line);
    }

    uint64_t now = NowMs();
    std::lock_guard<std::recursive_mutex> lk(s_mtx);

    /* connection watchdog: the session dropped (server gone, network
     * changed, Wi-Fi lost) -> show it; 30 s later leave the room locally */
    {   /* edge on the net thread's state: the welcome can be handled here
         * before the net thread marks the session Connected */
        static bool s_netUp = false;
        bool up = mp::GetState() == MpState::Connected;
        if (s_netUp && !up && s_connected) {
            s_connected = false;
            s_offlineSince = (int64_t)now;
            WriteLog("[MP-L] session lost (in room: %d)\n", (int)(s_code[0] != 0));
            if (s_code[0]) PushBanner("Connection lost - reconnecting...");
        }
        s_netUp = up;
    }
    if (!s_connected && s_code[0] && s_offlineSince > 0 &&
        (int64_t)now - s_offlineSince >= 30000)
        LocalLeave("Disconnected for 30 seconds - left the room");
    if (s_joinState == JOIN_PENDING) {
        if (!s_connected) JoinFail("Connection lost - try again");
        else if ((int64_t)now - s_joinAt >= kJoinTimeoutMs)
            JoinFail("No answer from the server - try again");
    }

    for (int i = 0; i < s_bannerCount; i++)
        s_banners[i].ttl -= 1.0f / 60.0f;
    while (s_bannerCount > 0 && s_banners[0].ttl <= 0.0f) {
        memmove(s_banners, s_banners + 1, sizeof(Banner) * (s_bannerCount - 1));
        s_bannerCount--;
    }

    /* emergency exit: Minus held 3s */
    {
        auto st = nn::hid::GetMergedNpadState();
        bool minus = (st.buttons & nn::hid::Minus) != 0;
        if (minus) {
            if (s_minusHeldAt < 0) s_minusHeldAt = now;
            if (now - s_minusHeldAt >= 3000 && s_phase != L_NONE) {
                s_minusHeldAt = now + 10000;
                ClearAllInput();
                if (s_code[0]) SendLine("{\"t\":\"lobby_leave\"}");
                ResetLobbyState();
                SetPhase(L_NONE, "");
                PushBanner("MP: emergency exit");
            }
        } else {
            s_minusHeldAt = -1;
        }
    }

    {   static bool prevOv = false;
        if (s_overlayOpen != prevOv) {
            prevOv = s_overlayOpen;
            if (s_overlayOpen) {
                s_relGuard = 0;
                s_relGuardStick = false;
                input::NavClear();   /* drop the edge that opened us */
                PushBlocks();        /* full block; base mask kept for close */
            } else {
                s_ovlClosedAt = NowMs();
                /* arm the release guard with what is held right now */
                s_relGuard = input::NavHeld() & ~nn::hid::Minus;
                int sx = 0, sy = 0;
                input::NavStick(&sx, &sy);
                s_relGuardStick = sx >= 8000 || sx <= -8000 ||
                                  sy >= 8000 || sy <= -8000;
                ApplyBlocks();
            }
        } else {
            GuardPump();
        }
    }

    /* menu model guard + telemetry/event consumption */
    menu::SetGuard(Guarded());
    {
        menu::Status ms = menu::Get(now);
        static int s_msPg = -1, s_msCur = -1;
        if (ms.page != s_msPg || ms.cursor != s_msCur) {
            if (s_msPg != -1) DbgMenu(ms.page, ms.cursor, "");
            s_msPg = ms.page; s_msCur = ms.cursor;
        }
        uint32_t ev = menu::ConsumeEvents();
        if (ev & menu::EV_RESUME) {
            DbgMenu(ms.page, ms.cursor, "resume");
            if (s_phase == L_PLAYING) {
                if (now - s_lastPauseReqMs >= 500) {
                    s_lastPauseReqMs = now;
                    CmdResumeReq();
                }
            } else {
                /* in a song outside a group round: resume locally */
                input::Tap(nn::hid::Plus, 60, 100, true);
            }
        }
        if (ev & menu::EV_RETRY_BLOCKED) {
            DbgMenu(ms.page, ms.cursor, "retry_blocked");
            if (now - s_blockBannerAt >= 2000) {
                s_blockBannerAt = now;
                PushBanner("Retry is off in multiplayer rooms%s",
                    ms.page == mp::menu::P_RES_MENU
                        ? " - press Down for Return to Song List" : "");
            }
        }
        if (ev & menu::EV_ITEM_BLOCKED) {
            DbgMenu(ms.page, ms.cursor, "item_blocked");
            if (now - s_blockBannerAt >= 2000) {
                s_blockBannerAt = now;
                PushBanner("Not available during a multiplayer round");
            }
        }
        if (ev & menu::EV_QUIT) {
            DbgMenu(ms.page, ms.cursor, "quit");
            s_quitting = true;
        }
    }

    /* expanded results panel: the first A on the results screen opens it
     * (the game never sees that press), A/B close it, L/R or Left/Right
     * flip its pages, Y opens it again. Admin taps count like real
     * presses; the auto-return driver's own taps don't. */
    {
        const bool can = s_resultsShownAt > 0 && s_code[0] &&
                         s_standingCount > 0;
        if (!can) {
            s_resPanel = 0; s_resPage = 0; s_resPanelAt = 0;
        } else if (s_autoReturn) {
            /* the driver is leaving: an open panel stays until the screen goes */
            if (s_resPanel == 1) { s_resPanel = 3; s_resPanelAt = 0; }
        } else {
            if (s_resPanel == 0) {
                s_resPanel = 1; s_resPanelAt = 0; s_resPage = 0;
                input::NavClear();
                ApplyBlocks();
            }
            const uint64_t e = input::NavConsume(
                nn::hid::A | nn::hid::B | nn::hid::Y | nn::hid::L |
                nn::hid::R | nn::hid::Left | nn::hid::Right);
            const int pages = (s_standingCount + kResPerPage - 1) / kResPerPage;
            if (s_resPage >= pages) s_resPage = pages - 1;
            if (s_resPanel == 2) {
                if (e & (nn::hid::A | nn::hid::B)) {
                    s_resPanel = 3; s_resPanelAt = (int64_t)now;
                    /* the closing press is still down: keep it from the
                     * game until it is released */
                    s_relGuard |= input::NavHeld() & (nn::hid::A | nn::hid::B);
                    ApplyBlocks();
                } else if (pages > 1 && (e & (nn::hid::R | nn::hid::Right))) {
                    s_resPage = (s_resPage + 1) % pages;
                } else if (pages > 1 && (e & (nn::hid::L | nn::hid::Left))) {
                    s_resPage = (s_resPage + pages - 1) % pages;
                }
            } else {
                const menu::Status rs = menu::Get(now);
                const bool early = rs.page == mp::menu::P_RES_SCORE ||
                                   rs.page == mp::menu::P_RES_VP;
                if ((s_resPanel == 1 && (e & nn::hid::A)) ||
                    (s_resPanel == 3 && (e & nn::hid::Y) &&
                     (kResReopenOnMenu || early))) {
                    s_resPanel = 2; s_resPanelAt = (int64_t)now;
                    ApplyBlocks();
                }
            }
        }
    }

    /* the A passed on the results menu produced no dest (driver or user):
     * resync the model to MENU on Return so input isn't swallowed forever */
    if (s_resultsShownAt > 0) {
        menu::Status ms = menu::Get(now);
        if (ms.page == mp::menu::P_RES_EXIT && ms.pageFrames > 240) {
            DbgMenu(ms.page, ms.cursor, "exit_timeout");
            menu::ExitFallback(now);
        }
    }

    /* results auto-return driver: owns the results menu once the round
     * is over; walks SCORE -> VP -> MENU -> Return to Song List */
    if (s_autoReturn && s_resultsShownAt > 0) {
        if (!s_retBannered) {
            s_retBannered = true;
            PushBanner("Round over - returning to the song list");
        }
        menu::Status ms = menu::Get(now);
        if (ms.ready && now - s_driveLastTap >= 300) {
            switch (ms.page) {
            case mp::menu::P_RES_SCORE:
                if (s_scoreTries >= 8) {
                    DbgMenu(ms.page, ms.cursor, "drive_give_up");
                    s_autoReturn = false;
                    PushBanner("Couldn't leave results - press A");
                    ApplyBlocks();
                } else {
                    s_scoreTries++;
                    DbgMenu(ms.page, ms.cursor, "drive_a");
                    s_driveLastTap = now;
                    input::Tap(nn::hid::A, 80, 80, true);
                }
                break;
            case mp::menu::P_RES_VP:
                DbgMenu(ms.page, ms.cursor, "drive_a");
                s_driveLastTap = now;
                input::Tap(nn::hid::A, 80, 80, true);
                break;
            case mp::menu::P_RES_MENU:
                if (ms.cursor == 0) {
                    DbgMenu(ms.page, ms.cursor, "drive_down");
                    s_driveLastTap = now;
                    input::Tap(nn::hid::Down, 80, 80, true);
                } else {
                    DbgMenu(ms.page, ms.cursor, "drive_a");
                    s_driveLastTap = now;
                    input::Tap(nn::hid::A, 80, 80, true);
                }
                break;
            default:
                break;
            }
        }
    }
    /* module customize opened from song select still counts as song select */
    if (!s_selActive && s_code[0] && CustomizeSelUi::IsInCustomizeScene())
        SetScene("sel");

    if (s_expectLobby && s_code[0] && now > s_expectLobbyAt) {
        s_expectLobby = false;
        ResetLobbyState();
        ClearAllInput();
        PushBanner("Lost the lobby");
    }

    if (s_destCheckAt > 0 && now >= s_destCheckAt) {
        s_destCheckAt = -1;
        bool inCust = CustomizeSelUi::IsInCustomizeScene() ||
                      CustomizeSelUi::IsOpen();
        if (s_phase == L_SELECT && !s_committed && !s_selActive && !inCust &&
            s_code[0] && strcmp(s_scene, "other") == 0)
            SendLine("{\"t\":\"lobby_leave\"}");
    }

    if (s_statePending && now - s_stateLastMs >= 50) {
        s_statePending = false;
        s_stateLastMs = now;
        char m[160];
        snprintf(m, sizeof(m),
            "{\"t\":\"state\",\"round\":%d,\"score\":%d,\"combo\":%d,"
            "\"pct\":%.2f,\"life\":%d,\"border\":%.2f}",
            s_round, s_curScore, s_curCombo, (double)s_curPct, s_curLife,
            (double)s_curBorder);
        SendLine(m);
        s_lastSentScore = s_curScore; s_lastSentCombo = s_curCombo;
        s_lastSentPct = s_curPct; s_lastSentLife = s_curLife;
    }


    if (s_phase == L_RESULTS && s_selActive && !s_backInSelSent &&
        s_code[0]) {
        s_backInSelSent = true;
        char m[64];
        snprintf(m, sizeof(m),
            "{\"t\":\"back_in_sel\",\"round\":%d}", s_round);
        SendLine(m);
    }

    if (s_readyDirty && s_code[0]) {
        s_readyDirty = false;
        char m[160];
        snprintf(m, sizeof(m),
            "{\"t\":\"ready\",\"pv\":%d,\"ready\":%s,\"reason\":\"%s\"}",
            s_readyPv, s_ready ? "true" : "false", s_readyReason);
        SendLine(m);
    }
}

/* ============================ hook points ========================== */

/* the song+difficulty clear % (e.g. 70 on this Extreme); the game's
 * running border clear_border_eff only reaches it at the last note */
static float ClearBorder(const PVGameData* pv) {
    return pv->clear_border > 0.0f ? pv->clear_border : pv->clear_border_eff;
}

/* final {.., border, clear, drop}: clear reflects the FINAL state (a run
 * that passed the border and then sank under it is NOT CLEAR); a song
 * quit from the pause menu never clears; drop = all life lost (in No Fail
 * Mode: the song ended with none left, the game's results say DROP x OUT) */
static void SendFinal(const PVGameData* pv, bool quit) {
    if (!pv) return;
    float border = ClearBorder(pv);
    bool drop = !quit && pv->life <= 0;
    bool clear = !quit && !drop && border > 0.0f &&
                 pv->percentage >= border;
    char m[320];
    snprintf(m, sizeof(m),
        "{\"t\":\"final\",\"round\":%d,\"score\":%d,\"pct\":%.2f,"
        "\"max_combo\":%d,\"jc\":[%d,%d,%d,%d,%d],\"life\":%d,"
        "\"border\":%.2f,\"clear\":%s,\"drop\":%s,\"quit\":%s,"
        "\"diff\":%d,\"ed\":%d}",
        s_playedRound, pv->score, (double)pv->percentage, pv->max_combo,
        pv->judge_count[0], pv->judge_count[1], pv->judge_count[2],
        pv->judge_count[3], pv->judge_count[4], pv->life, (double)border,
        clear ? "true" : "false", drop ? "true" : "false",
        quit ? "true" : "false", s_myD, s_myE);
    SendLine(m);
    strncpy(s_finalLine, m, sizeof(s_finalLine) - 1);
    s_finalLine[sizeof(s_finalLine) - 1] = 0;
    s_finalLineRound = s_playedRound;
    WriteLog("[MP-L] final r%d pct=%.2f border=%.2f/%.2f life=%d clear=%d "
             "drop=%d quit=%d\n", s_playedRound, (double)pv->percentage,
             (double)pv->clear_border, (double)pv->clear_border_eff,
             pv->life, (int)clear, (int)drop, (int)quit);
}

void OnPvSelInit() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    /* quit from the pause menu during a round: our final was never sent,
     * report the song we abandoned then wait like a finished player */
    if (s_code[0] && (s_phase == L_PLAYING || s_phase == L_DROPPED) &&
        s_playedRound > 0 && s_finalSentRound != s_playedRound) {
        s_finalSentRound = s_playedRound;
        SendFinal(GetPVGameData(), true);
        SetPhase(L_RESULTS, "Left the round - waiting for the others");
    }
    s_selActive = true;
    s_lastSelState = -1;
    s_autoReturn = false;
    s_quitting = false;
    s_retryWatch = false;
    s_retryRecStage = 0;
    s_prevPvPaused = 0;
    s_topRightBusy = false;
    menu::End();
    SetScene("sel");
    ApplyBlocks();
}
void OnPvSelDest() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    s_selActive = false;
    s_overlayOpen = false;
    SetScene("other");
    s_destCheckAt = NowMs() + 8000;
    ApplyBlocks();
}

/* The Random entry (cursor pv -2): the song the game rolled for it, from
 * the selector's random_pv slot; -1 while there is none. */
static int32_t RandomPv(const void* sel) {
    uintptr_t p = reinterpret_cast<uintptr_t>(sel);
    uintptr_t data2 = *reinterpret_cast<uintptr_t*>(p + 0x260D8);
    if (!data2) return -1;
    int32_t* e = *reinterpret_cast<int32_t**>(data2);
    return e ? *e : -1;
}

/* Resolve the follow target once per target/(diff,ed) change: the leader's
 * pv if selectable at any (d,e), else a name-matched pv that is, else
 * missing (no jumps, wrong_song). */
static void ResolveFollow(const void* sel) {
    s_folLeaderPv = s_localPv;
    s_folTarget = -1; s_folMissing = false;
    s_folFail = 0; s_folGaveUp = false; s_jumpFailedDiff = false;
    s_folTD = -1; s_folTE = -1;
    int32_t curD = *reinterpret_cast<const int32_t*>((const char*)sel + 0x261A0);
    int32_t curE = *reinterpret_cast<const int32_t*>((const char*)sel + 0x261A4);
    s_folLastD = curD; s_folLastE = curE;
    /* the difficulty we want to be on: ours, or the picker's when the
     * room says everyone plays the same one */
    int32_t wantD = curD, wantE = curE;
    bool forced = ForcedDiff(&wantD, &wantE);
    s_folWantD = forced ? wantD : -1; s_folWantE = forced ? wantE : 0;

    auto anySelectable = [&](int32_t p) {
        for (int d = 0; d < 4; d++)
            for (int e = 0; e < 2; e++)
                if (SongQuickJump::PvSelectableAt(sel, p, d, e)) return true;
        return false;
    };
    int32_t pv = s_localPv;
    if (!anySelectable(pv) && s_selName[0]) {
        int32_t fb = SongQuickJump::FindPvByName(s_selName);
        if (fb > 0 && anySelectable(fb)) pv = fb;
    }
    if (!anySelectable(pv)) {
        /* shown by the song-select tag (DrawNoSongTag) while it lasts */
        s_folMissing = true;
        return;
    }
    s_folTarget = pv;
    if (SongQuickJump::PvSelectableAt(sel, pv, wantD, wantE)) {
        s_folTD = wantD; s_folTE = wantE;
    } else if (SongQuickJump::ClosestSelectable(
                   sel, pv, wantD, wantE, &s_folTD, &s_folTE)) {
        /* once per (song, wanted, fallback): a forced target is resolved
         * again after every jump */
        int32_t sig[3] = { pv, wantD * 2 + wantE, s_folTD * 2 + s_folTE };
        if (memcmp(sig, s_folDiffBanner, sizeof(sig)) != 0) {
            memcpy(s_folDiffBanner, sig, sizeof(sig));
            PushBanner("No %s chart for %s - switched to %s",
                       DiffName(wantD, wantE),
                       s_selName[0] ? s_selName : "this song",
                       DiffName(s_folTD, s_folTE));
        }
    }
}

static void SendSelAck(bool has, bool ok, int32_t fbpv, const char* fbn) {
    char sig[120];
    snprintf(sig, sizeof(sig), "%d:%d:%d:%d:%d",
             s_leaderPv, (int)has, (int)ok, fbpv, (int)(fbpv > 0));
    if (!strncmp(sig, s_ackSig, sizeof(s_ackSig))) return;
    strncpy(s_ackSig, sig, sizeof(s_ackSig) - 1);
    char esc[80];
    mpjson::EscapeInto(esc, sizeof(esc), fbn);
    char m[240];
    snprintf(m, sizeof(m),
        "{\"t\":\"sel_ack\",\"pv\":%d,\"has\":%s,\"ok\":%s,"
        "\"fb_pv\":%d,\"fb_name\":\"%s\"}",
        s_leaderPv, has ? "true" : "false",
        ok ? "true" : "false", fbpv, esc);
    SendLine(m);
}

/* Jump to pv at the resolved (d,e): JumpSelectorToPvAt when it differs
 * from the selector's current ones, else the keep-difficulty jump. */
static void FollowJump(const void* sel, int32_t pv) {
    int32_t curD = *reinterpret_cast<const int32_t*>((const char*)sel + 0x261A0);
    int32_t curE = *reinterpret_cast<const int32_t*>((const char*)sel + 0x261A4);
    if (s_folTD >= 0 && (s_folTD != curD || s_folTE != curE))
        SongQuickJump::JumpSelectorToPvAt(
            const_cast<void*>(sel), pv, s_folTD, s_folTE);
    else
        SongQuickJump::JumpSelectorToPv(
            const_cast<void*>(sel), pv, true);
}

void OnPvSelFrame(const void* sel) {
    if (sel == nullptr) return;
    /* portrait prefetch: every 30 calls, warm the cache for the next
     * results screen (loads only happen on song-select frames) */
    {
        static int s_pf = 0;
        avatar::Ref refs[8]; int nm = 0;
        {
            std::lock_guard<std::recursive_mutex> g(s_mtx);
            if (s_code[0] && (s_pf++ % 10) == 0)
                for (int i = 0; i < s_memberCount && nm < 8; i++)
                    if (strcmp(s_members[i].cid, s_myCid) != 0)
                        refs[nm++] = s_members[i].av;
        }
        for (int i = 0; i < nm; i++) {
            void* t = nullptr; float a[2], b[2];
            avatar::Resolve(refs[i], &t, a, b);   /* marks what will be shown */
        }
        if (nm) avatar::Prefetch(avatar::LocalModule());
    }
    int32_t state  = *reinterpret_cast<const int32_t*>(
                        (const char*)sel + 0x68);
    int32_t selPv  = *reinterpret_cast<const int32_t*>(
                        (const char*)sel + 0x2617C);
    uint8_t busy   = *reinterpret_cast<const uint8_t*>(
                        (const char*)sel + 0x2587C);
    int32_t prevSt = s_lastSelState;
    uint64_t now = NowMs();
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    s_lastSelState = state;
    const int32_t curD = *reinterpret_cast<const int32_t*>(
                            (const char*)sel + 0x261A0);
    const int32_t curE = *reinterpret_cast<const int32_t*>(
                            (const char*)sel + 0x261A4);
    if (curD >= 0 && curD <= 3) {
        s_myD = curD; s_myE = (curD == 3 && curE == 1) ? 1 : 0;
        /* settle for a moment so a held D-pad sends one message */
        if ((int64_t)now - s_diffSentAt >= 250) ReportDiff();
    }

    /* follower lock follows the song list: drop it while the module picker
     * or customize is open, and keep buttons held at that moment blocked
     * until released so they don't land as fresh presses in the picker */
    {
        bool cust = CustomizeSelUi::IsInCustomizeScene() ||
                    CustomizeSelUi::IsOpen();
        bool browsing = state == 6 && !busy && !cust;
        if (browsing != s_selBrowsing) {
            bool wasLocked = IsFollowerMask(s_baseMask);
            s_selBrowsing = browsing;
            ApplyBlocks();
            if (wasLocked && !IsFollowerMask(s_baseMask)) {
                s_relGuard |= input::NavHeld() & MASK_FOLLOWER_SD;
                int sx = 0, sy = 0;
                input::NavStick(&sx, &sy);
                if (sx >= 8000 || sx <= -8000 || sy >= 8000 || sy <= -8000)
                    s_relGuardStick = true;
                PushBlocks();
            }
        }
    }

    /* sel_lists probe: dump the 8 sorted_pv_lists of the current sort */
    if (s_slPending) {
        s_slPending = false;
        int32_t sort = *reinterpret_cast<const int32_t*>(
                          (const char*)sel + 0x26198);
        int32_t diff = *reinterpret_cast<const int32_t*>(
                          (const char*)sel + 0x261A0);
        int32_t ed   = *reinterpret_cast<const int32_t*>(
                          (const char*)sel + 0x261A4);
        char buf[3072];
        int len = snprintf(buf, sizeof(buf),
            "{\"t\":\"dbg_sel_lists\",\"sort\":%d,\"diff\":%d,\"ed\":%d,"
            "\"lists\":[", sort, diff, ed);
        for (int d = 0; d < 4 && len > 0 && len < (int)sizeof(buf) - 60; d++) {
            for (int e = 0; e < 2 && len < (int)sizeof(buf) - 60; e++) {
                int idx = 8 * sort + 2 * d + e;
                auto* vec = reinterpret_cast<
                    const libcxx_vector_base<pvsel::PvData>*>(
                        (const char*)sel + 0x25DD0 + (uintptr_t)idx * 0x18);
                int n = -1;
                int has[24]; int nh = 0;
                if (vec && vec->begin_ && vec->end_ >= vec->begin_ &&
                    vec->end_ - vec->begin_ < 5000) {
                    n = (int)(vec->end_ - vec->begin_);
                    for (int i = 0; i < n; i++) {
                        const pvsel::PvData* pd = vec->begin_ + i;
                        if (!pd->data2 || !pd->data2->pv_db_entry) continue;
                        int32_t id = *pd->data2->pv_db_entry;
                        for (int j = 0; j < s_slN && nh < 24; j++)
                            if (s_slPvs[j] == id) has[nh++] = id;
                    }
                }
                int vis = SongQuickJump::SelVisibleCount(sel, d, e);
                len += snprintf(buf + len, sizeof(buf) - len,
                    "%s{\"d\":%d,\"e\":%d,\"n\":%d,\"sel\":%d,\"has\":[",
                    (d || e) ? "," : "", d, e, n, vis);
                for (int i = 0; i < nh; i++)
                    len += snprintf(buf + len, sizeof(buf) - len,
                        "%s%d", i ? "," : "", has[i]);
                len += snprintf(buf + len, sizeof(buf) - len, "]}");
            }
        }
        len += snprintf(buf + len, sizeof(buf) - len, "]}");
        WriteLog("[MP-L] sel_lists sort=%d diff=%d ed=%d\n", sort, diff, ed);
        SendLine(buf);
    }

    /* readiness */
    const char* reason = nullptr;
    bool inCust = CustomizeSelUi::IsInCustomizeScene() ||
                  CustomizeSelUi::IsOpen();
    if (inCust || busy) reason = "modules";
    else if (state != 6) reason = "not_in_sel";
    else if (s_jumpFailedDiff) reason = "difficulty";
    else if (!s_amLeader && s_localPv > 0 && s_folMissing)
        reason = "wrong_song";
    int32_t folPv = s_folTarget > 0 ? s_folTarget : s_localPv;
    bool ready = (state == 6 && !busy && !inCust &&
                  (s_amLeader || s_localPv <= 0 || selPv == folPv) &&
                  !s_jumpFailedDiff);
    int32_t rpv = (s_amLeader || s_localPv <= 0) ? selPv : folPv;
    if (ready != s_ready || rpv != s_readyPv ||
        strcmp(reason ? reason : "", s_readyReason) != 0) {
        s_ready = ready; s_readyPv = rpv;
        strncpy(s_readyReason, reason ? reason : "",
                sizeof(s_readyReason) - 1);
        s_readyReason[sizeof(s_readyReason) - 1] = 0;
        s_readyDirty = true;
    }

    /* reached song select after a round_end while away -> back to SELECT */
    if (s_phase == L_RESULTS && s_roundOver) {
        SetPhase(L_SELECT, s_amLeader ? "You're the leader - pick a song"
                                      : "Back in lobby - next round");
        ApplyBlocks();
    }

    /* leader: send sel on pv change */
    auto sendSel = [&](int32_t pv) {
        s_lastSelMs = now;
        s_lastSentSel = pv;
        s_unavailFor[0] = 0;
        char nm[48] = "";
        SongQuickJump::PvName(pv, nm, sizeof(nm));
        char esc[80]; mpjson::EscapeInto(esc, sizeof(esc), nm);
        char m[200];
        snprintf(m, sizeof(m),
            "{\"t\":\"sel\",\"pv\":%d,\"name\":\"%s\"}", pv, esc);
        SendLine(m);
    };
    if (s_phase == L_SELECT && s_amLeader && state == 6 && !busy) {
        if (selPv != -2 && selPv != s_lastSentSel &&
            now - s_lastSelMs >= 100)
            sendSel(selPv);
    }
    /* leader on the Random entry: the cursor's pv stays -2, the song the
     * game rolled is only known once A opened the confirm panel. Send it
     * then, so everyone else navigates to it before the start. */
    if (s_phase == L_SELECT && s_amLeader && state == 8 && selPv == -2) {
        int32_t rp = RandomPv(sel);
        if (rp > 0 && rp != s_lastSentSel && now - s_lastSelMs >= 100) {
            WriteLog("[MP-L] random pick -> pv %d\n", rp);
            sendSel(rp);
        }
    }

    /* leader: commit when the confirm panel starts the song (8 -> 10);
     * B on the song list leaves through 6 -> 10 -> Dest */
    if (s_phase == L_SELECT && s_amLeader && prevSt == 8 && state == 10 &&
        !s_committed) {
        s_committed = true;
        int32_t pv = selPv;
        if (pv == -2) {
            int32_t rp = RandomPv(sel);
            if (rp > 0) pv = rp;
        }
        char nm[48] = "";
        SongQuickJump::PvName(pv, nm, sizeof(nm));
        char esc[80]; mpjson::EscapeInto(esc, sizeof(esc), nm);
        char m[200];
        snprintf(m, sizeof(m),
            "{\"t\":\"start_req\",\"pv\":%d,\"name\":\"%s\"}", pv, esc);
        SendLine(m);
        SetPhase(L_LOADING, "Loading...");
        SetBlocks(MASK_ALL_NO_MINUS, false);
    }

    /* follower: re-resolve when the target or our difficulty changes */
    int32_t fwD = -1, fwE = 0;              /* the picker's, when forced */
    const bool forced = ForcedDiff(&fwD, &fwE);
    if (!forced) { fwD = -1; fwE = 0; }
    if (!s_amLeader && s_localPv > 0) {
        if (s_localPv != s_folLeaderPv ||
            curD != s_folLastD || curE != s_folLastE ||
            fwD != s_folWantD || fwE != s_folWantE) {
            ResolveFollow(sel);
            if (s_folMissing)
                SendSelAck(false, false, -1, "");
        }
    }
    /* follower: follow the leader's pick */
    const bool offDiff = forced && s_folTD >= 0 &&
                         (curD != s_folTD || curE != s_folTE);
    if (s_phase == L_SELECT && !s_amLeader && s_folTarget > 0 &&
        !s_folGaveUp && state == 6 && !busy &&
        (selPv != s_folTarget || offDiff) &&
        now - s_lastJumpMs >= 150) {
        s_lastJumpMs = now;
        s_lastJumpTarget = s_folTarget;
        s_jumpVerify = 4;
        FollowJump(sel, s_folTarget);
    }
    /* same_diff with no song to follow (the picker hasn't settled on one,
     * or this console lacks it): stay on our song and match the picker's
     * difficulty when our song has that chart */
    if (s_phase == L_SELECT && !s_amLeader && forced && s_folTarget <= 0 &&
        state == 6 && !busy && selPv > 0 && (curD != fwD || curE != fwE)) {
        int32_t sig = selPv * 8 + fwD * 2 + fwE;
        if (sig != s_diffSyncSig) { s_diffSyncSig = sig; s_diffSyncTries = 0; }
        if (s_diffSyncTries < 3 && (int64_t)now - s_diffSyncAt >= 400 &&
            SongQuickJump::PvSelectableAt(sel, selPv, fwD, fwE)) {
            s_diffSyncAt = (int64_t)now;
            s_diffSyncTries++;
            SongQuickJump::JumpSelectorToPvAt(
                const_cast<void*>(sel), selPv, fwD, fwE);
        }
    }
    /* jump verify + ack */
    if (s_jumpVerify > 0) {
        s_jumpVerify--;
        if (s_jumpVerify == 0) {
            bool ok = (selPv == s_lastJumpTarget) && !offDiff;
            if (ok) s_jumpFailedDiff = false;
            if (!ok && state == 6) s_folFail++;
            if (s_folFail >= 3 && !s_folGaveUp) {
                s_folGaveUp = true;
                s_jumpFailedDiff = true;
            }
            bool has = s_folTarget > 0;
            bool fb = (s_folTarget > 0 && s_folTarget != s_leaderPv);
            int32_t fbpv = fb ? s_folTarget : -1;
            char fbn[48] = "";
            if (fb) SongQuickJump::PvName(fbpv, fbn, sizeof(fbn));
            SendSelAck(has, ok, fbpv, fbn);
        }
    }

    /* LAUNCHING autopilot */
    if (s_phase == L_LAUNCHING) {
        bool inCust2 = CustomizeSelUi::IsInCustomizeScene() ||
                       CustomizeSelUi::IsOpen();
        if (s_launchStage == 0 && s_folMissing) {
            /* the song isn't on this console: spectate instead of
             * waiting blocked for the server's straggler drop */
            char m[64];
            snprintf(m, sizeof(m),
                "{\"t\":\"launch_fail\",\"round\":%d}", s_round);
            SendLine(m);
            SetPhase(L_SPECTATING, "Spectating (song not available)");
            ClearAllInput();
            PushBanner("You don't have this song - spectating");
        } else if (s_launchStage == 0) {
            if (state == 6 && !busy && !inCust2) {
                s_launchBlocking = true;
                SetBlocks(MASK_ALL_NO_MINUS, true);
                int32_t target = s_folTarget > 0 ? s_folTarget : s_localPv;
                /* same_diff: get onto the picker's difficulty first; after
                 * a few failed tries start on ours rather than miss the
                 * round */
                bool diffOff = offDiff && s_launchDiffTries < 4;
                if (selPv == target && !diffOff) {
                    input::Tap(nn::hid::A, 80, 80, true);
                    s_launchStage = 1;
                    s_launchStageAt = now;
                    s_launchTries = 0;
                } else if (s_folTarget > 0 &&
                           now - s_lastJumpMs >= 150) {
                    s_lastJumpMs = now;
                    if (selPv == target) s_launchDiffTries++;
                    FollowJump(sel, target);
                }
            }
        } else if (s_launchStage == 1) {
            /* A#1 sent: wait for the confirm panel (state 8) */
            if (state == 8) { s_launchStage = 2; s_launchStageAt = now; }
            else if (now - s_launchStageAt > 2000) {
                if (++s_launchTries > 3) s_launchStage = 3, s_launchStageAt = 0;
                else { input::Tap(nn::hid::A, 80, 80, true); s_launchStageAt = now; }
            }
        } else if (s_launchStage == 2) {
            /* the panel ignores input while it animates in */
            if (now - s_launchStageAt >= 700) {
                input::Tap(nn::hid::A, 80, 80, true);
                s_launchStage = 3; s_launchStageAt = now; s_launchTries = 0;
            }
        } else if (s_launchStage == 3) {
            if (state == 10) { s_launchStage = 4; }
            else if (state == 8 && s_launchStageAt && now - s_launchStageAt > 1500 &&
                     s_launchTries < 3) {
                s_launchTries++;
                input::Tap(nn::hid::A, 80, 80, true);
                s_launchStageAt = now;
            }
            else if (!s_launchStageAt || now - s_launchStageAt > 4000) {
                char m[64];
                snprintf(m, sizeof(m),
                    "{\"t\":\"launch_fail\",\"round\":%d}", s_round);
                SendLine(m);
                SetPhase(L_SPECTATING, "Spectating (launch failed)");
                ClearAllInput();
                PushBanner("Couldn't join this round - spectating");
            }
        }
    }
}

void OnPvGameFrame(PVGameData* pv) {
    if (pv == nullptr) return;
    uint64_t now = NowMs();
    std::lock_guard<std::recursive_mutex> lk(s_mtx);

    /* scene reporting */
    if (pv->loaded && s_resultsShownAt == 0 && !s_selActive)
        SetScene("game");

    s_curScore = pv->score;
    s_curCombo = pv->combo;
    s_curPct = pv->percentage;
    s_curLife = pv->life;
    s_curBorder = ClearBorder(pv);
    s_gaugeTick[0] = s_gaugeTick[1] = s_gaugeTick[2] = -1.0f;
    if (pv->loaded && pv->clear_border > 0.0f) {
        const float eff = pv->clear_border_eff;
        s_gaugeTick[0] = eff;
        s_gaugeTick[1] = eff * GaugeGreatPct() / pv->clear_border;
        s_gaugeTick[2] = eff * GaugeExcellentPct() / pv->clear_border;
    }
    s_gamePaused = pv->paused != 0;
    {
        bool trBusy = false;
        if (pv->loaded && !pv->paused && s_resultsShownAt == 0 &&
            g_current_game && g_current_game->play) {
            int steps = 0;
            for (PvGameTarget* t = g_current_game->target;
                 t != nullptr && steps < 64; t = t->next, steps++) {
                if (!t->note_active ||
                    (t->hit_state != 21 && t->hit_state != HitState_None))
                    continue;
                if ((t->target_pos.x > 1050.0f && t->target_pos.y < 160.0f) ||
                    (t->button_pos.x > 1050.0f && t->button_pos.y < 160.0f)) {
                    trBusy = true;
                    break;
                }
            }
        }
        s_topRightBusy = trBusy;
    }

    /* pause menu model on local pause edges (guarded rooms only) */
    {
        int paused = pv->paused != 0;
        if (paused != s_prevPvPaused) {
            s_prevPvPaused = paused;
            if (paused && Guarded() && !s_selActive &&
                s_resultsShownAt == 0) {
                menu::BeginPause(true, now);
                DbgMenu(mp::menu::P_PAUSE, 0, "open");
            } else if (!paused) {
                menu::Status ms = menu::Get(now);
                if (ms.page == mp::menu::P_PAUSE ||
                    ms.page == mp::menu::P_PAUSE_QUIT)
                    menu::End();
            }
        }
    }

    /* group pause reconciler: force the local pause to match the group.
     * forced Plus resumes from any menu item (probe P2). Off while the
     * start-resume is pending, while quitting, and during retry recovery */
    if (s_phase == L_PLAYING && Guarded() && pv->loaded && s_startSeen &&
        !s_resumePending && !s_quitting && s_retryRecStage == 0) {
        bool mismatch = (pv->paused != 0) != s_groupPaused;
        if (!mismatch || s_groupPaused != s_recTarget) s_recTaps = 0;
        s_recTarget = s_groupPaused;
        /* back off to one tap/2s after 5 taps without reaching the target */
        if (mismatch &&
            now - s_recLastTap >= (s_recTaps >= 5 ? 2000 : 600)) {
            s_recLastTap = now;
            s_recTaps++;
            DbgMenu(mp::menu::P_NONE, -1,
                    s_groupPaused ? "reconcile_pause" : "reconcile_resume");
            WriteLog("[MP-L] reconcile: paused=%d group=%d - tap Plus\n",
                     (int)(pv->paused != 0), (int)s_groupPaused);
            input::Tap(nn::hid::Plus, 60, 100, true);
        }
    }

    /* real Plus press -> group pause/resume req (guarded rooms; solo
     * rooms keep vanilla pause - no block, no req) */
    if (input::ConsumeRealPress(nn::hid::Plus)) {
        if (s_phase == L_PLAYING && Guarded() &&
            now - s_lastPauseReqMs >= 400) {
            s_lastPauseReqMs = now;
            char m[64];
            snprintf(m, sizeof(m),
                "{\"t\":\"%s\",\"round\":%d}",
                s_groupPaused ? "resume_req" : "pause_req", s_round);
            SendLine(m);
        }
    }

    if (s_phase == L_LOADING || s_phase == L_LAUNCHING) {
        if (pv->loaded && !s_apActive && !s_loadedSent) {
            s_apActive = true;
            s_apT0 = now;
            s_apLastTap = 0;
        }
        if (s_apActive) {
            if (pv->paused) {
                s_apActive = false;
                s_loadedSent = true;
                char m[96];
                snprintf(m, sizeof(m),
                    "{\"t\":\"loaded\",\"round\":%d,\"diff\":%d,\"ed\":%d}",
                    s_round, s_myD, s_myE);
                SendLine(m);
            } else if (now - s_apT0 > 3000) {
                s_apActive = false;
                s_loadedSent = true;
                char m[96];
                snprintf(m, sizeof(m),
                    "{\"t\":\"loaded\",\"round\":%d,\"diff\":%d,\"ed\":%d}",
                    s_round, s_myD, s_myE);
                SendLine(m);
            } else if (now - s_apLastTap >= 150) {
                input::Tap(nn::hid::Plus, 50, 100, true);
                s_apLastTap = now;
            }
        }
    }

    /* resume after start (or resume-if-paused when dropped) */
    if (s_resumePending && !s_apActive) {
        if (!pv->paused) {
            char m[96];
            snprintf(m, sizeof(m),
                "{\"t\":\"dbg_resume\",\"ok\":true,\"tries\":%d}",
                s_resumeTries);
            SendLine(m);
            WriteLog("[MP-L] resume ok after %d tries\n", s_resumeTries);
            s_resumePending = false;
        } else if (now - s_resumeLastMs >= 300) {
            s_resumeLastMs = now;
            if (++s_resumeTries > 5) {
                char m[96];
                snprintf(m, sizeof(m),
                    "{\"t\":\"dbg_resume\",\"ok\":false,\"tries\":%d}",
                    s_resumeTries - 1);
                SendLine(m);
                WriteLog("[MP-L] resume FAILED\n");
                s_resumePending = false;
            } else {
                input::Tap(nn::hid::Plus, 50, 100, true);
            }
        }
    }

    /* retry watch (guarded rooms): armed by OnStageResult(false); a song
     * clock that keeps advancing with no pv_sel init means an A slipped
     * through to Retry - leave via the pause menu instead */
    if (s_retryWatch && !s_selActive) {
        if (pv->loaded) {
            uint64_t st = pv->pv_data.script_time;
            if (st != s_retryStLast) {
                s_retryStLast = st;
                if (s_retryStChangeAt == 0) s_retryStChangeAt = now;
                else if (now - s_retryStChangeAt >= 500) {
                    s_retryWatch = false;
                    s_retryRecStage = 1;
                    s_retryRecTries = 0;
                    s_retryRecLastTap = 0;
                    s_retryRecRounds = 0;
                    DbgMenu(mp::menu::P_NONE, -1, "retry_detected");
                    PushBanner("Retry isn't allowed in multiplayer - "
                               "returning to the song list");
                    SendLine("{\"t\":\"dbg_warn\",\"w\":\"retry_recovery\"}");
                }
            }
        }
    }

    /* retry recovery: pause the re-running song and leave through the
     * pause menu's Return to Song List (Up wraps to the bottom item) */
    if (s_retryRecStage > 0) {
        menu::Status ms = menu::Get(now);
        switch (s_retryRecStage) {
        case 1:                     /* tap Plus until the pause opens */
            if (pv->paused) {
                s_retryRecStage = 2;
            } else if (now - s_retryRecLastTap >= 500) {
                if (++s_retryRecTries > 10) {
                    DbgMenu(mp::menu::P_NONE, -1, "recovery_give_up");
                    s_retryRecStage = 0;
                } else {
                    s_retryRecLastTap = now;
                    input::Tap(nn::hid::Plus, 60, 100, true);
                }
            }
            break;
        case 2:                     /* menu open c0: Up -> Return (c4) */
            if (ms.page == mp::menu::P_PAUSE && ms.ready) {
                DbgMenu(ms.page, ms.cursor, "recovery_up");
                input::Tap(nn::hid::Up, 60, 100, true);
                s_retryRecStage = 3;
            }
            break;
        case 3:                     /* cursor on Return + ready -> A */
            if (ms.page == mp::menu::P_PAUSE && ms.cursor == 4 &&
                ms.ready) {
                DbgMenu(ms.page, ms.cursor, "recovery_a");
                input::Tap(nn::hid::A, 60, 100, true);
                s_retryRecStage = 4;
                s_retryRecT0 = now;
            }
            break;
        case 4:                     /* wait for OnPvSelInit */
            if (now - s_retryRecT0 >= 5000 && s_retryRecRounds < 1) {
                if (!pv->paused) {   /* song is running again: retry once */
                    DbgMenu(mp::menu::P_NONE, -1, "recovery_restart");
                    s_retryRecRounds++;
                    s_retryRecStage = 1;
                    s_retryRecTries = 0;
                    s_retryRecLastTap = 0;
                } else {
                    s_retryRecT0 = now;   /* menu open: keep waiting */
                }
            }
            break;
        }
    }

    if (s_phase == L_PLAYING && s_statePending == false &&
        (pv->score != s_lastSentScore || pv->combo != s_lastSentCombo ||
         pv->life != s_lastSentLife || pv->percentage != s_lastSentPct))
        s_statePending = true;
}

void OnStageResult(bool init) {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (init) {
        SetScene("results");
        if (s_code[0]) {
            s_resultsShownAt = NowMs();
            menu::BeginResults(Guarded(), s_resultsShownAt);
            s_retBannered = false;
            s_scoreTries = 0;
        }
    }
    if (init && s_code[0] && s_playedRound > 0 &&
        s_finalSentRound != s_playedRound) {
        s_finalSentRound = s_playedRound;
        SendFinal(GetPVGameData(), false);
        SetPhase(L_RESULTS, "Round over - waiting in results");
        s_resultInitAt = NowMs();
        input::ResetAll();     /* drop queued taps/spam: an A here = RETRY */
        ApplyBlocks();
    } else if (!init) {
        if (s_phase == L_RESULTS) s_resultInitAt = 0;
        s_resultsShownAt = 0;
        s_autoReturn = false;
        menu::End();
        /* guarded rooms: watch for a song clock that keeps running -
         * that means an A slipped through to Retry */
        if (Guarded()) {
            s_retryWatch = true;
            s_retryStChangeAt = 0;
            PVGameData* pv = GetPVGameData();
            s_retryStLast = pv ? pv->pv_data.script_time : 0;
        }
        SetScene("other");
        /* new round's standings replace the ones on the results screen */
        if (s_launchHandledRound > s_standingsRound) s_standingCount = 0;
        ApplyBlocks();
    }
}

/* ============================ UI data ============================== */

bool InLobby() { return s_code[0] != 0; }
bool PanelWanted() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    return s_overlayOpen ||
           (s_selActive && (s_lastSelState == 6 || s_lastSelState == 8));
}

void GetSnapshot(Snapshot* out) {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    memset(out, 0, sizeof(*out));
    out->phase = (int)s_phase;
    out->connected = s_connected;
    out->joinState = s_joinState;
    strncpy(out->joinCode, s_joinCode, sizeof(out->joinCode) - 1);
    strncpy(out->joinErr, s_joinErr, sizeof(out->joinErr) - 1);
    strncpy(out->webKey, s_webKey, sizeof(out->webKey) - 1);
    strncpy(out->webUrl, s_webUrl, sizeof(out->webUrl) - 1);
    out->offlineMs = (!s_connected && s_offlineSince > 0)
        ? (int32_t)((int64_t)NowMs() - s_offlineSince) : -1;
    out->inSongSelect = s_selActive;
    strncpy(out->code, s_code, sizeof(out->code) - 1);
    strncpy(out->myCid, s_myCid, sizeof(out->myCid) - 1);
    strncpy(out->leaderCid, s_leaderCid, sizeof(out->leaderCid) - 1);
    strncpy(out->statusLine, s_statusLine, sizeof(out->statusLine) - 1);
    strncpy(out->notReady, s_notReady, sizeof(out->notReady) - 1);
    strncpy(out->unavailFor, s_unavailFor, sizeof(out->unavailFor) - 1);
    out->selPv = s_selPv;
    strncpy(out->selName, s_selName, sizeof(out->selName) - 1);
    out->memberCount = s_memberCount;
    for (int i = 0; i < s_memberCount; i++) {
        MemberInfo& d = out->members[i];
        strncpy(d.cid, s_members[i].cid, sizeof(d.cid) - 1);
        strncpy(d.name, s_members[i].name, sizeof(d.name) - 1);
        strncpy(d.reason, s_members[i].reason, sizeof(d.reason) - 1);
        strncpy(d.role, s_members[i].role, sizeof(d.role) - 1);
        d.connected = s_members[i].connected;
        d.is_leader = strcmp(s_members[i].cid, s_leaderCid) == 0;
        d.ready = s_members[i].ready;
        d.score = s_members[i].score;
        d.pct = s_members[i].pct;
        d.av = s_members[i].av;
        d.srPos = s_members[i].sr[0];
        d.wins = s_members[i].sr[1];
        for (int k = 0; k < 3; k++) d.places[k] = s_members[i].sr[2 + k];
        d.plat = s_members[i].plat;
        d.diff = s_members[i].diff; d.ed = s_members[i].ed;
        /* flags only mean something while a song is being picked; our
         * own comes straight from the selector (no server round trip) */
        d.nosong = s_phase == L_SELECT && !d.is_leader &&
            (strcmp(s_members[i].cid, s_myCid) == 0
                 ? (!s_amLeader && s_localPv > 0 && s_folMissing)
                 : s_members[i].nosong);
    }
    out->sameDiff = s_sameDiff;
    out->noFail = s_noFail;
    for (int q = 0; q < 3; q++) out->gaugeTick[q] = s_gaugeTick[q];
    out->resPanel = s_resPanel; out->resPage = s_resPage;
    out->resPerPage = kResPerPage;
    out->resPanelMs = s_resPanelAt > 0
        ? (int32_t)((int64_t)NowMs() - s_resPanelAt) : -1;
    {   menu::Status rs = menu::Get(NowMs());
        out->resCanReopen = kResReopenOnMenu ||
            rs.page == mp::menu::P_RES_SCORE || rs.page == mp::menu::P_RES_VP; }
    out->myDiff = s_myD; out->myEd = s_myE;
    int lc = 0, pc = 0;
    for (int i = 0; i < s_memberCount; i++)
        if (strcmp(s_members[i].role, "player") == 0) {
            pc++;
            if (s_members[i].loaded) lc++;
        }
    out->loadedCount = lc; out->playerCount = pc;
    out->bannerCount = s_bannerCount;
    for (int i = 0; i < s_bannerCount; i++) {
        strncpy(out->banners[i].text, s_banners[i].text,
                sizeof(out->banners[i].text) - 1);
        out->banners[i].ttl = s_banners[i].ttl;
    }
    out->boardCount = s_boardCount;
    for (int i = 0; i < s_boardCount; i++) {
        strncpy(out->board[i].cid, s_board[i].cid,
                sizeof(out->board[i].cid) - 1);
        strncpy(out->board[i].name, s_board[i].name,
                sizeof(out->board[i].name) - 1);
        out->board[i].score = s_board[i].score;
        out->board[i].pct = s_board[i].pct;
        out->board[i].life = s_board[i].life;
        out->board[i].rank = s_board[i].rank;
        out->board[i].connected = s_board[i].connected;
        out->board[i].me = strcmp(s_board[i].cid, s_myCid) == 0;
    }
    out->standingsComplete = s_standingsComplete;
    out->standingCount = s_standingCount;
    for (int i = 0; i < s_standingCount; i++) {
        auto& r = s_standings[i]; auto& d = out->standing[i];
        strncpy(d.cid, r.cid, sizeof(d.cid) - 1);
        strncpy(d.name, r.name, sizeof(d.name) - 1);
        d.score = r.score; d.pct = r.pct; d.max_combo = r.max_combo;
        d.life = r.life; d.rank = r.rank; d.final = r.final;
        d.connected = r.connected;
        d.diff = r.diff; d.ed = r.ed;
        for (int q = 0; q < 5; q++) d.jc[q] = r.jc[q];
        d.hasJc = r.hasJc; d.border = r.border; d.clear = r.clear;
        d.drop = r.drop; d.autoplay = r.autoplay; d.plat = PLAT_UNKNOWN;
        d.me = r.cid[0] ? (strcmp(r.cid, s_myCid) == 0)
                        : (strstr(r.name, s_myName) != nullptr && s_myName[0]);
        d.av = avatar::Ref{};
        for (int q = 0; q < s_memberCount; q++)
            if (r.cid[0] && strcmp(s_members[q].cid, r.cid) == 0) {
                d.av = s_members[q].av; d.plat = s_members[q].plat; break; }
    }
    out->resultsAgeMs = s_resultsShownAt > 0
        ? (int32_t)(NowMs() - s_resultsShownAt) : -1;
    out->myPct = s_curPct;
    out->sinceStartMs = s_launchStartMs > 0
        ? (int32_t)(NowMs() - s_launchStartMs) : -1;
    /* a countdown whose `start` never came (round cancelled, we left, or
     * we're back in song select) must not linger */
    bool cdLive = s_countdownEndMs > 0 && !s_selActive &&
                  (int64_t)NowMs() < s_countdownEndMs + 4000;
    out->countdownMs = cdLive ? (int32_t)(s_countdownEndMs - NowMs()) : -1;
    if (cdLive && out->countdownMs < 0) out->countdownMs = 0;
    out->countdownAgeMs = cdLive
        ? (int32_t)(NowMs() - s_countdownStartMs) : -1;
    out->goMs = s_goAt > 0 ? (int32_t)(NowMs() - s_goAt) : -1;
    if (out->goMs > 800) out->goMs = -1;
    out->inSong = strcmp(s_scene, "game") == 0;
    out->topRightBusy = s_topRightBusy;
    /* the leader waits in LOADING, followers in LAUNCHING once loaded */
    out->waitingStart = s_phase == L_LOADING ||
                        (s_phase == L_LAUNCHING &&
                         (s_launchStage >= 4 || s_loadedSent));
    out->round = s_round;
    strncpy(out->ownerCid, s_ownerCid, sizeof(out->ownerCid) - 1);
    strncpy(out->pickMode, s_pickMode, sizeof(out->pickMode) - 1);
    strncpy(out->assignedCid, s_assignedCid, sizeof(out->assignedCid) - 1);
    out->groupPaused = s_groupPaused;
    out->voteAgreed = s_voteAgreed; out->voteNeed = s_voteNeed;
    out->voteEndsIn = s_voteEndsIn;
    strncpy(out->voteKind, s_voteKind, sizeof(out->voteKind) - 1);
    out->minusHeld = s_minusHeldAt >= 0 && s_code[0] &&
                     s_phase != L_NONE;
    out->minusHeldMs = s_minusHeldAt >= 0
        ? (int)(NowMs() - s_minusHeldAt) : 0;
}

void Init() {
    s_notifyStyle = Config::mpNotifyStyle == "text" ? 1
                  : Config::mpNotifyStyle == "off" ? 2 : 0;
    strncpy(s_myCid, Config::mpClientId.c_str(), sizeof(s_myCid) - 1);
    strncpy(s_myName, Config::mpPlayerName.c_str(), sizeof(s_myName) - 1);
}

/* panel/dbg commands */
/* never queued while offline: the request would reach the server
 * whenever the session comes back, long after it was asked for */
void CmdLobbyCreate() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (!s_connected) {
        PushBanner("Not connected to the server");
        return;
    }
    SendLine("{\"t\":\"lobby_create\"}");
}

void DbgSelLists(const int32_t* pvs, int n) {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    int m = n > 24 ? 24 : n;
    for (int i = 0; i < m; i++) s_slPvs[i] = pvs[i];
    s_slN = m;
    s_slPending = true;
}
void CmdLobbyJoin(const char* code, bool quiet) {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (s_joinState == JOIN_PENDING) return;   /* one at a time */
    char up[8] = "";
    int n = 0;
    bool ok = code && code[0];
    for (; code && code[n] && n < 7; n++) {
        char ch = code[n];
        up[n] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
        /* letters / digits only (also keeps the JSON line valid) */
        if (!((up[n] >= 'A' && up[n] <= 'Z') || (up[n] >= '0' && up[n] <= '9')))
            ok = false;
    }
    memcpy(s_joinCode, up, sizeof(s_joinCode));
    s_joinQuiet = quiet;
    if (!ok) { JoinFail("Room codes are 4 letters"); return; }
    if (!s_connected) {
        /* nothing was asked, so nothing failed: the join screen shows the
         * offline state for as long as it lasts */
        s_joinState = JOIN_IDLE;
        WriteLog("[MP-L] join '%s' not sent: offline\n", s_joinCode);
        if (!quiet || !s_overlayOpen) PushBanner("Not connected to the server");
        return;
    }
    char m[80];
    snprintf(m, sizeof(m), "{\"t\":\"lobby_join\",\"code\":\"%s\"}", up);
    SendLine(m);
    s_joinState = JOIN_PENDING;
    s_joinErr[0] = 0;
    s_joinAt = (int64_t)NowMs();
}
void CmdWebKey() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (!s_connected || !s_code[0] || strcmp(s_ownerCid, s_myCid) != 0) return;
    SendLine("{\"t\":\"web_key\"}");
}
void CmdLobbyLeave() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (!s_connected && s_code[0]) { LocalLeave("Left the room"); return; }
    SendLine("{\"t\":\"lobby_leave\"}");
}
void LeaveForServerSwitch() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (!s_code[0]) return;
    if (s_connected) SendLine("{\"t\":\"lobby_leave\"}");
    LocalLeave("Server changed - left the room");
}
bool OverlayOpen() { return s_overlayOpen; }
void ToggleOverlay() { s_overlayOpen = !s_overlayOpen; }
void SetOverlayOpen(bool open) {
    s_overlayOpen = open;
}
/* ms timestamp of the last overlay close (-1 = none); used by Hints to
 * ignore the ZR press that closed us for a short window. */
int64_t OverlayClosedAt() { return s_ovlClosedAt; }

void CmdPickMode(const char* mode, const char* cid) {
    char m[120];
    if (cid && *cid)
        snprintf(m, sizeof(m),
            "{\"t\":\"set_pick_mode\",\"mode\":\"%s\",\"cid\":\"%s\"}",
            mode, cid);
    else
        snprintf(m, sizeof(m),
            "{\"t\":\"set_pick_mode\",\"mode\":\"%s\"}", mode);
    SendLine(m);
}
void CmdSameDiff(bool on) {
    SendLine(on ? "{\"t\":\"set_opt\",\"key\":\"same_diff\",\"value\":true}"
                : "{\"t\":\"set_opt\",\"key\":\"same_diff\",\"value\":false}");
}
void CmdNoFail(bool on) {
    SendLine(on ? "{\"t\":\"set_opt\",\"key\":\"no_fail\",\"value\":true}"
                : "{\"t\":\"set_opt\",\"key\":\"no_fail\",\"value\":false}");
}
int RoundNoFail() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (!s_code[0]) return -1;
    return s_roundNoFail >= 0 ? s_roundNoFail : s_noFail;
}
void CmdKick(const char* cid) {
    if (!cid || !*cid) return;
    char m[80];
    snprintf(m, sizeof(m), "{\"t\":\"kick\",\"cid\":\"%.20s\"}", cid);
    SendLine(m);
}
void CmdPauseReq() {
    char m[64];
    snprintf(m, sizeof(m), "{\"t\":\"pause_req\",\"round\":%d}", s_round);
    SendLine(m);
}
void CmdResumeReq() {
    char m[64];
    snprintf(m, sizeof(m), "{\"t\":\"resume_req\",\"round\":%d}", s_round);
    SendLine(m);
}

void ReportAutoPlay(bool on) {
    if (!s_code[0] && s_phase == L_NONE) return;
    char m[64];
    snprintf(m, sizeof(m),
             "{\"t\":\"autoplay\",\"on\":%s,\"pct\":%d,\"mode\":\"%s\"}",
             on ? "true" : "false", (int)AutoPlay::EffectivePct(),
             AutoPlay::ModeName());
    SendLine(m);
}

void CmdSetName(const char* name) {
    if (!name || !*name) return;
    Config::mpPlayerName = name;
    Config::Save();
    char esc[80];
    mpjson::EscapeInto(esc, sizeof(esc), name);
    char m[140];
    snprintf(m, sizeof(m), "{\"t\":\"set_name\",\"name\":\"%s\"}", esc);
    SendLine(m);
}

} // namespace mp::lobby
