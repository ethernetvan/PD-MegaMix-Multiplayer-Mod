#pragma once
#include <stdint.h>

/* Lobby avatars: the module in the player's Module Shared Set A, slot 1.
 *
 * Local module: read straight from the save manager while in song select
 * (polled, so a module change is picked up without opening any screen) and
 * announced to the server as {"t":"avatar","module":N}.
 *
 * Portraits: the game's own module-select image (spr set SPR_SEL_MD%03dCMN,
 * sprite ..._MD_IMG) is loaded on the game thread, read back from the GPU
 * once on the present thread, CPU-decoded (BC5 "YCbCr": mip0 = Y/A,
 * mip1 = Cb/Cr) into an owned RGBA texture and cached per module id. */
namespace mp::avatar {

/* Save hooks pass the save-data manager; keeps the latest non-null. */
void CaptureSaveMgr(void* mgr);

/* Polled Shared Set A slot-1 module id; -1 until known. */
int32_t LocalModule();

/* Server welcome: (re)announce the local module. */
void OnWelcome();

/* pv_sel frame (game thread): module poll + sprite-set loads. */
void GamePump();
/* OnFrame (present thread): staging readback, decode, upload, frees. */
void RenderPump();
/* D3DInit: the device/context the GPU work runs on. */
void SetDevice(void* dev, void* ctx);

/* UI thread. Portrait for `module`; marks it wanted.
 * Returns 1 = ready (*tex / uv0..uv1 = square crop), 0 = loading,
 * -1 = unavailable (custom module / decode failed / module < 0). */
int Get(int32_t module, void** tex, float uv0[2], float uv1[2]);

/* Mark a module wanted without drawing (warm the cache before a round). */
void Prefetch(int32_t module);

/* A player's avatar as announced through the server. */
struct Ref {
    int32_t  module = -1;
    int32_t  chara = -1;      /* the module's character on the owner's console */
    uint32_t mh = 0;          /* owner's module-table signature, 0 unknown */
    char     img[13] = "";    /* owner's uploaded portrait id, "" none */
};
/* Which portrait to show for `r`, in order: this console's own image of
 * the module (only if its table entry matches the owner's), the owner's
 * uploaded portrait (only with [multiplayer] remote_avatars = true), the
 * character's default module, else -1 (silhouette). Same return values
 * as Get(). */
int Resolve(const Ref& r, void** tex, float uv0[2], float uv1[2]);

/* net thread: avatar_px {img, rgba} (err = refused). Only accepted for an
 * id this console asked for; the payload must be exactly 96x96 RGBA. */
void OnRemotePixels(const char* img, const char* b64, int n, bool err);
/* remote_avatars switched off: free every downloaded portrait */
void DropRemote();

/* debug (admin dbg_cmd): overwrite Set A slot 1 / report state / write the
 * module's whole decoded card to ExlSD:/av_dump_MMM_WxH.rgba (in pv_sel). */
void DbgPoke(int32_t module);
void DbgPeek();
void DbgDump(int32_t module);

}
