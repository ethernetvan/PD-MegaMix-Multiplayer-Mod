#include <share.h>
#include "MpLog.hpp"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>

// Logging must never block a game thread: console output stalls forever
// while the console is in QuickEdit "Select" mode (one click on it), and a
// game thread stuck in WriteLog freezes the whole game (white, hung window;
// verified 2026-11-18). WriteLog only appends to a ring under a short lock;
// a dedicated thread does all file/console I/O. When the ring is full the
// new lines are dropped and counted.
static FILE* s_fp = nullptr;
static std::mutex s_mu;
static constexpr size_t kRing = 256 * 1024;
static char   s_ring[kRing];
static size_t s_head = 0, s_len = 0;      /* guarded by s_mu */
static unsigned s_dropped = 0;            /* guarded by s_mu */
static HANDLE s_wake = nullptr;
static std::atomic<bool> s_started{false};

FILE* MpLogFile() { return s_fp; }

static void LogThread() {
    static char chunk[16 * 1024];
    for (;;) {
        WaitForSingleObject(s_wake, 250);
        for (;;) {
            size_t n = 0;
            unsigned dropped = 0;
            {   std::lock_guard<std::mutex> lk(s_mu);
                n = s_len < sizeof(chunk) ? s_len : sizeof(chunk);
                for (size_t i = 0; i < n; i++)
                    chunk[i] = s_ring[(s_head + i) % kRing];
                s_head = (s_head + n) % kRing;
                s_len -= n;
                dropped = s_dropped; s_dropped = 0;
            }
            if (!n && !dropped) break;
            if (s_fp) {
                if (n) fwrite(chunk, 1, n, s_fp);
                if (dropped) fprintf(s_fp, "[MP] log: %u lines dropped\n", dropped);
                fflush(s_fp);
            }
            /* console echo: may block for as long as the console is
             * paused - only this thread waits */
            if (n && GetConsoleWindow()) {
                fwrite(chunk, 1, n, stdout);
                fflush(stdout);
            }
        }
    }
}

void MpLogInit(const wchar_t* modDir) {
    if (!modDir) return;
    std::wstring p = modDir;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    p += L"mp_log.txt";
    if (s_fp) { fclose(s_fp); s_fp = nullptr; }
    s_fp = _wfsopen(p.c_str(), L"wb", _SH_DENYNO);
    if (!s_started.exchange(true)) {
        s_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread(LogThread).detach();
    }
}

void WriteLog(const char* fmt, ...) {
    if (!fmt) return;
    char buf[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int h = snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d ", t.wHour,
                     t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + h, sizeof(buf) - h, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    n += h;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    {   std::lock_guard<std::mutex> lk(s_mu);
        if (s_len + (size_t)n > kRing) { s_dropped++; return; }
        size_t tail = (s_head + s_len) % kRing;
        for (int i = 0; i < n; i++) s_ring[(tail + i) % kRing] = buf[i];
        s_len += (size_t)n;
    }
    if (s_wake) SetEvent(s_wake);
}
