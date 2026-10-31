#include "MpLobby.hpp"
#include "MpNet.hpp"
#include "MpJson.hpp"
#include "MpInput.hpp"
#include "../Config.hpp"
#include "../logger.hpp"
#include "../overlays/SongQuickJump.hpp"
#include "../nc/ui/customize_sel.hpp"
#include "../hid.hpp"
#include <nn.hpp>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <deque>
#include <string>

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
};
static MRec s_members[8];
static int  s_memberCount = 0;

static bool  s_connected = false;
static bool  s_amLeader  = false;
static bool  s_amPlayer  = false;
static bool  s_committed = false;
static bool  s_selActive = false;
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
static bool  s_retryWarned = false;
static int   s_retStage = 0;
static int64_t s_retStageAt = 0;
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
static int32_t s_lastSentScore = -1, s_lastSentCombo = -1,
               s_lastSentLife = -1;
static float   s_lastSentPct = -1.0f;

struct Banner { char text[128]; float ttl; };
static Banner s_banners[4];
static int  s_bannerCount = 0;

struct BoardRow { char cid[24]; char name[40]; int32_t score; float pct;
                  bool connected; };
static BoardRow s_board[8];
static int  s_boardCount = 0;
struct StandingRow { char name[40]; int32_t score; float pct;
                     int32_t max_combo; int32_t life; int rank; bool final; };
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

static std::deque<std::string> s_inQ;
static std::mutex s_inQMtx;

static bool    s_inLobbyDirty();
static void    ApplyBlocks();

static uint64_t NowMs() {
    return (uint64_t)nn::os::GetSystemTick().GetInt64Value() * 1000
           / nn::os::GetSystemTickFrequency();
}

static void SendLine(const char* m) { mp::Enqueue(m); }

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

static void SetPhase(LPhase p, const char* status) {
    if (p != s_phase)
        WriteLog("[MP-L] phase %d -> %d\n", (int)s_phase, (int)p);
    s_phase = p;
    strncpy(s_statusLine, status, sizeof(s_statusLine) - 1);
    s_statusLine[sizeof(s_statusLine) - 1] = 0;
}

static void SetBlocks(uint64_t mask, bool zeroLs) {
    input::SetBlocked(mask);
    input::SetZeroLStick(zeroLs);
}
static void ClearAllInput() { input::ResetAll(); }

static const uint64_t MASK_FOLLOWER =
    nn::hid::A | nn::hid::Up | nn::hid::Down | nn::hid::L | nn::hid::R |
    nn::hid::Y;
static const uint64_t MASK_ALL_NO_MINUS = ~(uint64_t)nn::hid::Minus;

/* ============================== inbound ============================ */

void PushLine(const char* line, int len) {
    std::lock_guard<std::mutex> lk(s_inQMtx);
    if (s_inQ.size() >= 64) s_inQ.pop_front();
    s_inQ.emplace_back(line, len);
}

static void ResetLobbyState() {
    s_phase = L_NONE;
    s_code[0] = 0; s_leaderCid[0] = 0;
    s_memberCount = 0; s_boardCount = 0; s_standingCount = 0;
    s_selPv = -1; s_leaderPv = -1; s_localPv = -1;
    s_committed = false; s_amPlayer = false;
    s_loadedSent = false; s_startSeen = false;
    s_apActive = false; s_statePending = false;
    s_launchStage = 0; s_jumpVerify = 0;
    s_jumpFailedDiff = false;
    s_launchBlocking = false; s_amLeader = false;
    s_resumePending = false;
    s_standingsComplete = false; s_backInSelSent = false;
    s_resultInitAt = 0; s_retryWarned = false;
    s_notReady[0] = 0;
    s_ready = true; s_readyReason[0] = 0;
    s_statusLine[0] = 0;
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
    {   int gi2 = mpjson::Find(line, tok, ntok, "group_paused");
        if (gi2 >= 0) s_groupPaused = line[tok[gi2].start] == 't'; }
    int32_t rd = 0; gi("round", &rd); s_round = rd;
    int32_t spv = -1; if (gi("sel_pv", &spv)) s_selPv = spv;
    s_amLeader = strcmp(s_leaderCid, s_myCid) == 0;

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
        if ((j = mpjson::Find(line, tok + i, end - i, "pct")) >= 0) {
            char pb[24] = "";
            mpjson::Copy(line, tok[i + j], pb, sizeof(pb));
            b.pct = (float)strtod(pb, nullptr);
        }
        if ((j = mpjson::Find(line, tok + i, end - i, "connected")) >= 0)
            b.connected = line[tok[i + j].start] == 't';
        s_boardCount++;
        i = end;
    }
}

static void HandleStandings(const char* line, jsmntok_t* tok, int ntok) {
    int mi = mpjson::Find(line, tok, ntok, "e");
    if (mi < 0 || tok[mi].type != JSMN_ARRAY) return;
    s_standingCount = 0;
    int i = mi + 1;
    while (i < ntok && s_standingCount < 8 && tok[i].type == JSMN_OBJECT) {
        int end = i + mpjson::Skip(tok, i, ntok), j;
        StandingRow& r = s_standings[s_standingCount];
        memset(&r, 0, sizeof(r));
        if ((j = mpjson::Find(line, tok + i, end - i, "name")) >= 0)
            mpjson::Copy(line, tok[i + j], r.name, sizeof(r.name));
        int v;
        if ((j = mpjson::Find(line, tok + i, end - i, "score")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v)) r.score = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "pct")) >= 0) {
            char pb[24] = "";
            mpjson::Copy(line, tok[i + j], pb, sizeof(pb));
            r.pct = (float)strtod(pb, nullptr);
        }
        if ((j = mpjson::Find(line, tok + i, end - i, "max_combo")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v)) r.max_combo = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "life")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v)) r.life = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "rank")) >= 0 &&
            mpjson::ToInt(line, tok[i + j], &v)) r.rank = v;
        if ((j = mpjson::Find(line, tok + i, end - i, "final")) >= 0)
            r.final = line[tok[i + j].start] == 't';
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

static void ProcessLine(const std::string& l) {
    char line[2048];
    strncpy(line, l.c_str(), sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    jsmn_parser ps; jsmn_init(&ps);
    jsmntok_t tok[256];
    int ntok = jsmn_parse(&ps, line, strlen(line), tok, 256);
    if (ntok < 1 || tok[0].type != JSMN_OBJECT) return;
    int ti = mpjson::Find(line, tok, ntok, "t");
    char t[24] = "";
    if (ti >= 0) mpjson::Copy(line, tok[ti], t, sizeof(t));

    std::lock_guard<std::recursive_mutex> lk(s_mtx);

    if (strcmp(t, "welcome") == 0) {
        s_connected = true;
        if (s_code[0]) { s_expectLobby = true; s_expectLobbyAt = NowMs() + 3000; }
        if (s_inLobbyDirty()) s_readyDirty = true;
        return;
    }
    if (strcmp(t, "lobby") == 0) {
        HandleLobby(line, tok, ntok);
        s_expectLobby = false;
        if (s_phase == L_NONE || s_phase == L_DROPPED)
            SetPhase(strcmp(s_serverPhase, "SELECT") == 0
                     ? L_SELECT : L_SPECTATING,
                     s_amLeader ? "You're the leader - pick a song"
                                : "In lobby - waiting for the leader");
        ApplyBlocks();
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
            char nm[48] = "";
            SongQuickJump::PvName(pv, nm, sizeof(nm));
            PushBanner("Leader picked: %s", nm[0] ? nm : s_selName);
        }
        return;
    }
    if (strcmp(t, "sel_status") == 0) {
        s_notReady[0] = 0;
        int off = 0;
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
                off += snprintf(s_notReady + off, sizeof(s_notReady) - off,
                                "%s%s%s; ", nm, rs[0] ? ":" : "", rs);
                i = end;
            }
        }
        return;
    }
    if (strcmp(t, "launch") == 0) {
        int32_t rd = 0, i = mpjson::Find(line, tok, ntok, "round"), v;
        if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) rd = v;
        if (s_launchHandledRound == rd) return;
        s_launchHandledRound = rd;
        s_round = rd;
        int32_t pv = 0; i = mpjson::Find(line, tok, ntok, "pv");
        if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) pv = v;
        s_selPv = pv;
        if (s_amLeader || s_committed) {
            s_amPlayer = true;
            SetPhase(L_LOADING, "Loading...");
            SetBlocks(MASK_ALL_NO_MINUS, false);
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
                if (!SongQuickJump::PvInDb(pv) && s_selName[0]) {
                    int32_t fb = SongQuickJump::FindPvByName(s_selName);
                    if (fb > 0) s_localPv = fb;
                }
                SetPhase(L_LAUNCHING, "Leader started - finish module "
                                      "selection to join");
                s_launchStage = 0;
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
    if (strcmp(t, "start") == 0) {
        int32_t rd = 0, i = mpjson::Find(line, tok, ntok, "round"), v;
        if (i >= 0 && mpjson::ToInt(line, tok[i], &v)) rd = v;
        if (s_startSeen && rd == s_round) return;
        s_startSeen = true;
        s_round = rd;
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
        PushBanner("%s by %s", p ? "Paused" : "Resumed",
                   nm[0] ? nm : by);
        if (p) {
            /* pause ourselves if not already paused */
            if (!s_gamePaused) input::Tap(nn::hid::Plus, 60, 100);
            ApplyBlocks();
        } else {
            /* resume via the retry-safe mechanism (only taps while paused) */
            s_resumePending = true;
            s_resumeTries = 0;
            s_resumeLastMs = 0;
            ApplyBlocks();
        }
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
        s_committed = false; s_startSeen = false; s_loadedSent = false;
        s_jumpFailedDiff = false; s_standingsComplete = false;
        s_backInSelSent = false; s_resultInitAt = 0;
        s_amLeader = strcmp(nl, s_myCid) == 0;
        SetPhase(L_SELECT, s_amLeader ? "You're the leader - pick a song"
                                      : "Back in lobby - next round");
        ApplyBlocks();
        if (s_amLeader) PushBanner("You're the leader - pick a song");
        else PushBanner("Round over - back in song select");
        return;
    }
    if (strcmp(t, "event") == 0) {
        char k[24] = "", nm[40] = "";
        int i = mpjson::Find(line, tok, ntok, "kind");
        if (i >= 0) mpjson::Copy(line, tok[i], k, sizeof(k));
        i = mpjson::Find(line, tok, ntok, "name");
        if (i >= 0) mpjson::Copy(line, tok[i], nm, sizeof(nm));
        if (strcmp(k, "combo") == 0) PushBanner("%s hit a big combo!", nm);
        else if (strcmp(k, "low_life") == 0) PushBanner("%s is in danger!", nm);
        else if (strcmp(k, "leader") == 0) PushBanner("%s is the leader", nm);
        else if (strcmp(k, "joined") == 0) PushBanner("%s joined", nm);
        else if (strcmp(k, "left") == 0) PushBanner("%s left", nm);
        else if (strcmp(k, "disconnected") == 0) PushBanner("%s disconnected", nm);
        else if (strcmp(k, "reconnected") == 0) PushBanner("%s reconnected", nm);
        return;
    }
}

static bool s_inLobbyDirty() {
    return s_phase == L_SELECT || s_phase == L_LAUNCHING ||
           s_phase == L_LOADING || s_phase == L_PLAYING ||
           s_phase == L_SPECTATING || s_phase == L_RESULTS;
}

static void ApplyBlocks() {
    if (s_phase == L_SELECT && !s_amLeader)
        SetBlocks(MASK_FOLLOWER, true);
    else if (s_phase == L_LOADING ||
             (s_phase == L_LAUNCHING && s_launchBlocking))
        SetBlocks(MASK_ALL_NO_MINUS, false);
    else if (s_phase == L_RESULTS)
        SetBlocks(0, false);                  /* results: user navigates */
    else if (s_phase == L_PLAYING) {
        if (s_groupPaused)
            SetBlocks(nn::hid::Plus | nn::hid::A | nn::hid::B | nn::hid::X |
                      nn::hid::Y | nn::hid::Up | nn::hid::Down |
                      nn::hid::Left | nn::hid::Right, false);
        else
            SetBlocks(nn::hid::Plus, false);
    }
    else
        SetBlocks(0, false);
}

/* ============================ per-frame ============================ */

void Pump() {
    for (;;) {
        std::string l;
        {
            std::lock_guard<std::mutex> g(s_inQMtx);
            if (s_inQ.empty()) break;
            l = std::move(s_inQ.front());
            s_inQ.pop_front();
        }
        ProcessLine(l);
    }

    uint64_t now = NowMs();
    std::lock_guard<std::recursive_mutex> lk(s_mtx);

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
            if (s_overlayOpen) SetBlocks(MASK_ALL_NO_MINUS, true);
            else ApplyBlocks();
        }
    }

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
            s_code[0])
            SendLine("{\"t\":\"lobby_leave\"}");
    }

    if (s_statePending && now - s_stateLastMs >= 50) {
        s_statePending = false;
        s_stateLastMs = now;
        char m[128];
        snprintf(m, sizeof(m),
            "{\"t\":\"state\",\"round\":%d,\"score\":%d,\"combo\":%d,"
            "\"pct\":%.2f,\"life\":%d}",
            s_round, s_curScore, s_curCombo, (double)s_curPct, s_curLife);
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

void OnPvSelInit() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    s_selActive = true;
}
void OnPvSelDest() {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    s_selActive = false;
    s_destCheckAt = NowMs() + 1500;
}

void OnPvSelFrame(const void* sel) {
    if (sel == nullptr) return;
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

    /* readiness */
    const char* reason = nullptr;
    bool inCust = CustomizeSelUi::IsInCustomizeScene() ||
                  CustomizeSelUi::IsOpen();
    if (inCust || busy) reason = "modules";
    else if (state != 6) reason = "not_in_sel";
    else if (s_jumpFailedDiff) reason = "difficulty";
    else if (s_localPv > 0 && selPv != s_localPv &&
             !SongQuickJump::PvInDb(s_localPv)) reason = "wrong_song";
    bool ready = (state == 6 && !busy && !inCust &&
                  (s_localPv <= 0 || selPv == s_localPv) &&
                  !s_jumpFailedDiff);
    int32_t rpv = s_localPv > 0 ? s_localPv : selPv;
    if (ready != s_ready || rpv != s_readyPv ||
        (!ready && reason && strcmp(reason, s_readyReason))) {
        s_ready = ready; s_readyPv = rpv;
        if (reason) strncpy(s_readyReason, reason, sizeof(s_readyReason) - 1);
        else s_readyReason[0] = 0;
        s_readyDirty = true;
    }

    /* leader: send sel on pv change */
    if (s_phase == L_SELECT && s_amLeader && state == 6 && !busy) {
        if (selPv != -2 && selPv != s_lastSentSel &&
            now - s_lastSelMs >= 100) {
            s_lastSelMs = now;
            s_lastSentSel = selPv;
            char nm[48] = "";
            SongQuickJump::PvName(selPv, nm, sizeof(nm));
            char esc[80]; mpjson::EscapeInto(esc, sizeof(esc), nm);
            char m[200];
            snprintf(m, sizeof(m),
                "{\"t\":\"sel\",\"pv\":%d,\"name\":\"%s\"}", selPv, esc);
            SendLine(m);
        }
    }

    /* leader: commit on 8->10 */
    if (s_phase == L_SELECT && s_amLeader && prevSt == 8 && state == 10 &&
        !s_committed) {
        s_committed = true;
        int32_t pv = selPv;
        if (pv == -2) {
            uintptr_t p = reinterpret_cast<uintptr_t>(sel);
            uintptr_t data2 = *reinterpret_cast<uintptr_t*>(p + 0x260D8);
            if (data2) {
                int32_t* e = *reinterpret_cast<int32_t**>(data2);
                if (e) pv = *e;
            }
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

    /* follower: follow the leader's pick */
    if (s_phase == L_SELECT && !s_amLeader && s_localPv > 0 &&
        state == 6 && !busy && selPv != s_localPv &&
        now - s_lastJumpMs >= 150) {
        int32_t target = s_localPv;
        if (!SongQuickJump::PvInDb(target) && s_selName[0]) {
            int32_t fb = SongQuickJump::FindPvByName(s_selName);
            if (fb > 0) target = fb;
        }
        if (target > 0 && SongQuickJump::PvInDb(target)) {
            s_lastJumpMs = now;
            s_lastJumpTarget = target;
            s_localPv = target;
            s_jumpVerify = 4;
            SongQuickJump::JumpSelectorToPv(
                const_cast<void*>(sel), target, true);
        }
    }
    /* jump verify + ack */
    if (s_jumpVerify > 0) {
        s_jumpVerify--;
        if (s_jumpVerify == 0) {
            bool ok = (selPv == s_lastJumpTarget);
            s_jumpFailedDiff = !ok && state == 6;
            bool has = SongQuickJump::PvInDb(s_leaderPv);
            bool fb = (s_localPv != s_leaderPv);
            int32_t fbpv = fb ? s_localPv : -1;
            char fbn[48] = "";
            if (fb) SongQuickJump::PvName(fbpv, fbn, sizeof(fbn));
            char sig[120];
            snprintf(sig, sizeof(sig), "%d:%d:%d:%d:%d",
                     s_leaderPv, (int)has, (int)ok, fbpv, (int)fb);
            if (strncmp(sig, s_ackSig, sizeof(s_ackSig))) {
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
            if (!ok)
                PushBanner("Not available at your difficulty - "
                           "change difficulty with < >");
        }
    }

    /* LAUNCHING autopilot */
    if (s_phase == L_LAUNCHING) {
        bool inCust2 = CustomizeSelUi::IsInCustomizeScene() ||
                       CustomizeSelUi::IsOpen();
        if (s_launchStage == 0) {
            if (state == 6 && !busy && !inCust2) {
                s_launchBlocking = true;
                SetBlocks(MASK_ALL_NO_MINUS, true);
                if (selPv == s_localPv) {
                    input::Tap(nn::hid::A, 80, 80);
                    s_launchStage = 1;
                    s_launchStageAt = now;
                    s_launchTries = 0;
                } else if (now - s_lastJumpMs >= 150) {
                    s_lastJumpMs = now;
                    SongQuickJump::JumpSelectorToPv(
                        const_cast<void*>(sel), s_localPv, true);
                }
            }
        } else if (s_launchStage == 1) {
            /* A#1 sent: wait for the confirm panel (state 8) */
            if (state == 8) { s_launchStage = 2; s_launchStageAt = now; }
            else if (now - s_launchStageAt > 2000) {
                if (++s_launchTries > 3) s_launchStage = 3, s_launchStageAt = 0;
                else { input::Tap(nn::hid::A, 80, 80); s_launchStageAt = now; }
            }
        } else if (s_launchStage == 2) {
            /* the panel ignores input while it animates in */
            if (now - s_launchStageAt >= 700) {
                input::Tap(nn::hid::A, 80, 80);
                s_launchStage = 3; s_launchStageAt = now; s_launchTries = 0;
            }
        } else if (s_launchStage == 3) {
            if (state == 10) { s_launchStage = 4; }
            else if (state == 8 && s_launchStageAt && now - s_launchStageAt > 1500 &&
                     s_launchTries < 3) {
                s_launchTries++;
                input::Tap(nn::hid::A, 80, 80);
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

    s_curScore = pv->score;
    s_curCombo = pv->combo;
    s_curPct = pv->percentage;
    s_curLife = pv->life;
    s_gamePaused = pv->paused != 0;

    /* real Plus press while Plus is blocked -> group pause/resume req */
    if (s_phase == L_PLAYING &&
        input::ConsumeRealPress(nn::hid::Plus)) {
        char m[64];
        snprintf(m, sizeof(m),
            "{\"t\":\"%s\",\"round\":%d}",
            s_groupPaused ? "resume_req" : "pause_req", s_round);
        SendLine(m);
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
                char m[64];
                snprintf(m, sizeof(m),
                    "{\"t\":\"loaded\",\"round\":%d}", s_round);
                SendLine(m);
            } else if (now - s_apT0 > 3000) {
                s_apActive = false;
                s_loadedSent = true;
                char m[64];
                snprintf(m, sizeof(m),
                    "{\"t\":\"loaded\",\"round\":%d}", s_round);
                SendLine(m);
            } else if (now - s_apLastTap >= 150) {
                input::Tap(nn::hid::Plus, 50, 100);
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
                input::Tap(nn::hid::Plus, 50, 100);
            }
        }
    }

    /* landed in a retry while in RESULTS -> report once per round */
    if (s_phase == L_RESULTS && pv->loaded && pv->percentage == 0.0f &&
        pv->score == 0 && s_resultInitAt > 0 && !s_retryWarned) {
        s_retryWarned = true;
        WriteLog("[MP-L] WARN: song reloaded during RESULTS (retry hit)\n");
        SendLine("{\"t\":\"dbg_warn\",\"w\":\"retry_during_results\"}");
    }

    if (s_phase == L_PLAYING && s_statePending == false &&
        (pv->score != s_lastSentScore || pv->combo != s_lastSentCombo ||
         pv->life != s_lastSentLife || pv->percentage != s_lastSentPct))
        s_statePending = true;
}

void OnStageResult(bool init) {
    std::lock_guard<std::recursive_mutex> lk(s_mtx);
    if (init && s_phase == L_PLAYING) {
        PVGameData* pv = GetPVGameData();
        if (pv) {
            char m[256];
            snprintf(m, sizeof(m),
                "{\"t\":\"final\",\"round\":%d,\"score\":%d,\"pct\":%.2f,"
                "\"max_combo\":%d,\"jc\":[%d,%d,%d,%d,%d],\"life\":%d}",
                s_round, pv->score, (double)pv->percentage, pv->max_combo,
                pv->judge_count[0], pv->judge_count[1], pv->judge_count[2],
                pv->judge_count[3], pv->judge_count[4], pv->life);
            SendLine(m);
        }
        SetPhase(L_RESULTS, "Round over - waiting in results");
        s_resultInitAt = NowMs();
        s_retryWarned = false;
        s_retStage = 0; s_retStageAt = 0;
        input::ResetAll();     /* drop queued taps/spam: an A here = RETRY */
        SetBlocks(MASK_ALL_NO_MINUS, false);
    } else if (!init && s_phase == L_RESULTS) {
        s_resultInitAt = 0;
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
    strncpy(out->code, s_code, sizeof(out->code) - 1);
    strncpy(out->myCid, s_myCid, sizeof(out->myCid) - 1);
    strncpy(out->leaderCid, s_leaderCid, sizeof(out->leaderCid) - 1);
    strncpy(out->statusLine, s_statusLine, sizeof(out->statusLine) - 1);
    strncpy(out->notReady, s_notReady, sizeof(out->notReady) - 1);
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
    }
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
        strncpy(out->board[i].name, s_board[i].name,
                sizeof(out->board[i].name) - 1);
        out->board[i].score = s_board[i].score;
        out->board[i].pct = s_board[i].pct;
        out->board[i].connected = s_board[i].connected;
        out->board[i].me = strcmp(s_board[i].cid, s_myCid) == 0;
    }
    out->standingsComplete = s_standingsComplete;
    out->standingCount = s_standingCount;
    for (int i = 0; i < s_standingCount; i++) {
        auto& r = s_standings[i]; auto& d = out->standing[i];
        strncpy(d.name, r.name, sizeof(d.name) - 1);
        d.score = r.score; d.pct = r.pct; d.max_combo = r.max_combo;
        d.life = r.life; d.rank = r.rank; d.final = r.final;
        d.me = strstr(r.name, s_myName) != nullptr && s_myName[0];
    }
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
    strncpy(s_myCid, Config::mpClientId.c_str(), sizeof(s_myCid) - 1);
    strncpy(s_myName, Config::mpPlayerName.c_str(), sizeof(s_myName) - 1);
}

/* panel/dbg commands */
void CmdLobbyCreate() { SendLine("{\"t\":\"lobby_create\"}"); }
void CmdLobbyJoin(const char* code) {
    char m[80];
    char up[8] = "";
    for (int i = 0; code && code[i] && i < 6; i++)
        up[i] = (code[i] >= 'a' && code[i] <= 'z') ? code[i] - 32 : code[i];
    snprintf(m, sizeof(m), "{\"t\":\"lobby_join\",\"code\":\"%s\"}", up);
    SendLine(m);
}
void CmdLobbyLeave() { SendLine("{\"t\":\"lobby_leave\"}"); }
bool OverlayOpen() { return s_overlayOpen; }
void ToggleOverlay() { s_overlayOpen = !s_overlayOpen; }
void SetOverlayOpen(bool open) { s_overlayOpen = open; }

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
