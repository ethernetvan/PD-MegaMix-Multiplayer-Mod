#pragma once
#include <stdint.h>

/* The game's merged module table (gm_module_tbl: vanilla + DLC + mods'
 * mod_gm_module_tbl). Captured from its parser so a console can tell
 * whether it has a module id at all, which character it belongs to and
 * whether its entry matches another console's (same id, other mod). */
namespace mp::moduledb {

/* exl_main: hook the table parser (main+0x3c3980) to remember the db. */
void Init();
/* first song-select frame: the table is complete from here on */
void MarkReady();
/* the table was captured and looks sane */
bool Available();

struct Info { int32_t id, chara, sortIndex, cos; uint32_t sig; };
/* false: no such module on this console, or the table isn't ready yet.
 * sig = FNV-1a 32 over chara, cos, sort_index (LE int32 each), never 0:
 * language-independent (names are localized), and differs when a mod
 * reuses an official id (Mikudayo pack: 437/438 are official T-shirt
 * modules on a console without the pack). */
bool Find(int32_t module, Info* out);

/* chara 0..9 (MIKU RIN LEN LUKA NERU HAKU KAITO MEIKO SAKINE TETO) ->
 * that character's default module; -1 for anything else */
int32_t DefaultModuleFor(int32_t chara);

/* debug (dbg_cmd module_dump): ExlSD:/module_tbl_dump.txt */
void DbgDump();

}
