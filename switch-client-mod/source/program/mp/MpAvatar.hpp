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
/* Top of nvnImguiCalc (present thread): readback, decode, upload, frees. */
void RenderPump();

/* UI thread. Portrait for `module`; marks it wanted.
 * Returns 1 = ready (*tex / uv0..uv1 = square crop), 0 = loading,
 * -1 = unavailable (custom module / decode failed / module < 0). */
int Get(int32_t module, void** tex, float uv0[2], float uv1[2]);

/* Mark a module wanted without drawing (warm the cache before a round). */
void Prefetch(int32_t module);

/* debug (admin dbg_cmd): overwrite Set A slot 1 / report state / write the
 * module's whole decoded card to ExlSD:/av_dump_MMM_WxH.rgba (in pv_sel). */
void DbgPoke(int32_t module);
void DbgPeek();
void DbgDump(int32_t module);

}
