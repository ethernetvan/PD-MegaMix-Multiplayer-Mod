// PC port: avatar stub for H1. No module-table read and no GPU readback yet,
// so every portrait resolves to "no image" and the UI draws its silhouette
// fallback. Announce {"t":"avatar","module":-1,"chara":-1,"mh":0}.
#include "MpAvatar.hpp"
#include "MpNet.hpp"
#include "../platform/MpLog.hpp"

namespace mp::avatar {

void CaptureSaveMgr(void*) {}
int32_t LocalModule() { return -1; }

void OnWelcome() {
    mp::Enqueue("{\"t\":\"avatar\",\"module\":-1,\"chara\":-1,\"mh\":0}");
}

void GamePump() {}
void RenderPump() {}

int Get(int32_t, void**, float[2], float[2]) { return -1; }
void Prefetch(int32_t) {}
int Resolve(const Ref&, void**, float[2], float[2]) { return -1; }
void OnRemotePixels(const char*, const char*, int, bool) {}
void DropRemote() {}
void DbgPoke(int32_t) {}
void DbgPeek() {}
void DbgDump(int32_t) {}

} // namespace mp::avatar
