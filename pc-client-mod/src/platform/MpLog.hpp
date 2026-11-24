#pragma once
// PC port: mod logging (replaces Switch logger.hpp). Writes mp_log.txt in the
// mod directory (one line flushed per call) and echoes to the DML console.
#include <cstdarg>
#include <cstdio>

void MpLogInit(const wchar_t* modDir);
FILE* MpLogFile();
void WriteLog(const char* fmt, ...);
