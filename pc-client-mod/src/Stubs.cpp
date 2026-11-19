// PC port stubs — see Stubs.hpp.
#include "Stubs.hpp"
#include "platform/MpLog.hpp"
#include <cstring>

namespace moduledb {
bool Available() { return false; }
void MarkReady() {}
const Info* Find(int32_t) { return nullptr; }
int32_t DefaultModuleFor(int32_t) { return -1; }
void DbgDump() { WriteLog("[MP] moduledb: stub (no module table on PC)\n"); }
} // namespace moduledb

namespace Hints {
void ProcessInput(uint64_t, uint64_t) {}
void SetPvSelActive(bool) {}
void SetSongConfirmActive(bool) {}
} // namespace Hints
