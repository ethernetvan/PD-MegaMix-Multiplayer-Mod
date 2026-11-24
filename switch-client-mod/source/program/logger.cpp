#include "fs.hpp"
#include "lib.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

static nn::fs::FileHandle s_LogHandle;
static bool s_LogFileReady = false;
static int64_t s_CurrentFileOffset = 0;

/* thread_local is unusable here (emits __aarch64_read_tp which no loaded
 * module exports). */
static bool s_InsideLog = false;
static std::recursive_mutex s_LogMutex;
static bool s_AppendMode = false;
static const int64_t kPreallocSize = 0x100000;   /* 1MB write-only cap */
static const int64_t kAppendCap    = 0x800000;   /* 8MB append cap */

void InitLoggerOnce() {
    if (s_LogFileReady) return;

    const char* logPath = "ExlSD:/hook_log.txt";

    nn::fs::DeleteFile(logPath);
    nn::fs::CreateFile(logPath, 0);

    /* Append mode works on hardware; Ryujinx rejects that OpenFile, so
     * fall back to a preallocated file opened Write-only. */
    Result res = nn::fs::OpenFile(&s_LogHandle, logPath,
                                nn::fs::OpenMode_Write | nn::fs::OpenMode_Append);
    s_AppendMode = R_SUCCEEDED(res);
    if (!s_AppendMode) {
        nn::fs::CreateFile(logPath, kPreallocSize);
        res = nn::fs::OpenFile(&s_LogHandle, logPath, nn::fs::OpenMode_Write);
    }
    if (R_SUCCEEDED(res)) {
        s_LogFileReady = true;
        s_CurrentFileOffset = 0;
    }
}

void WriteLog(const char* format, ...) {
    std::lock_guard<std::recursive_mutex> lock(s_LogMutex);
    if (s_InsideLog) return;            /* same-thread recursion */
    s_InsideLog = true;

    char logBuffer[512];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(logBuffer, sizeof(logBuffer), format, args);
    va_end(args);

    if (len > 0) {
        InitLoggerOnce();

        if (s_LogFileReady) {
            /* Write-only mode has a hard cap: writing past EOF on a file
             * opened without Append aborts (2002-6201) -> game crash. */
            if (!s_AppendMode && s_CurrentFileOffset + len > kPreallocSize - 16) {
                static const char kFull[] = "[log full]\n";
                if (s_CurrentFileOffset < kPreallocSize - (int64_t)sizeof(kFull)) {
                    nn::fs::WriteFile(s_LogHandle, s_CurrentFileOffset,
                                      kFull, sizeof(kFull) - 1,
                                      nn::fs::WriteOption::CreateOption(0));
                    s_CurrentFileOffset += sizeof(kFull) - 1;
                }
                s_InsideLog = false;
                return;
            }
            if (s_AppendMode && s_CurrentFileOffset > kAppendCap) {
                s_InsideLog = false;
                return;                 /* silently drop past 8MB */
            }
            nn::fs::WriteFile(
                s_LogHandle,
                s_CurrentFileOffset,
                logBuffer,
                len,
                nn::fs::WriteOption::CreateOption(0)
            );
            s_CurrentFileOffset += len;
        }
    }

    s_InsideLog = false;
}
