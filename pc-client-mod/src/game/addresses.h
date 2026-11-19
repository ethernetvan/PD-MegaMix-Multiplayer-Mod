#pragma once
// PC addresses for DivaMegaMix.exe — Mega Mix+ 1.03, Steam buildid 11004484.
// Absolute VAs (image base 0x140000000; the exe always loads at its preferred
// base). Each line names the Switch-side equivalent this port replaces.

namespace addr {

/* --- PV game task (per-frame telemetry) --- */
constexpr size_t TaskPvGameInit = 0x1405DA040;      // Switch TaskPvGame init hook
constexpr size_t TaskPvGameCtrl = 0x1405DA060;      // Switch TaskPvGame ctrl hook
constexpr size_t TaskPvGameDest = 0x1405DA0A0;      // Switch TaskPvGame dest hook
constexpr size_t GetPVGameData  = 0x140266720;      // Switch GetPVGameData (PVGameData*)

/* --- Song selectors --- */
/* MegaMix-style selector (PVSelectorSwitch) */
constexpr size_t PvSelSwitchInit      = 0x1406ED9D0; // Switch PVSelectorSwitch::Init
constexpr size_t PvSelSwitchCtrl      = 0x1406EDC40; // Switch PVSelectorSwitch::Ctrl
constexpr size_t PvSelSwitchDest      = 0x1406EE720; // Switch PVSelectorSwitch::Dest
constexpr size_t PvSelSwitchCreateList= 0x1406F3100; // Switch CreateSortedPVList
constexpr size_t PvSelSwitchChangeSort= 0x1406F2F40; // Switch ChangeSortFilter
constexpr size_t PvSelSwitchPertains  = 0x1406F3D40; // Switch CheckSongPertains
constexpr size_t PvSelSwitchPlaylist  = 0x1406D8D50; // Switch playlist-mode helper
/* Future Tone-style selector (PVselPS4) */
constexpr size_t PvSelPS4Init         = 0x140202D50; // Switch PS4-selector Init
constexpr size_t PvSelPS4Ctrl         = 0x1402033C0; // Switch PS4-selector Ctrl
constexpr size_t PvSelPS4Dest         = 0x140204DA0; // Switch PS4-selector Dest
constexpr size_t PvSelPS4CreateList   = 0x140206C30; // Switch PS4 CreateSortedPVList
constexpr size_t PvSelPS4ChangeSort   = 0x1402078D0; // Switch PS4 ChangeSortFilter
constexpr size_t PvSelPS4Pertains     = 0x1402079E0; // Switch PS4 CheckSongPertains
constexpr size_t PVListSetSelectedIndex = 0x140217500; // Switch SelPvList::SetSelectedIndex
constexpr size_t IsFutureToneMode     = 0x1414AB9E3; // bool: FT (PS4) UI mode

/* --- Results --- */
constexpr size_t StageResultSwitchInit = 0x14064C0E0; // Switch StageResultSwitch::Init
constexpr size_t StageResultSwitchDest = 0x14064C300; // Switch StageResultSwitch::Dest
constexpr size_t StageResultPS4Init    = 0x140231A70; // Switch StageResultPS4::Init
constexpr size_t StageResultPS4Dest    = 0x140232DD0; // Switch StageResultPS4::Dest
constexpr size_t CAetControllerInLoopOutSetLayer = 0x14065E7D0; // Switch AET layer-name signal (win_vp_in)
constexpr size_t IsInSongResults       = 0x1401E8090; // Switch IsInSongResults

/* --- PV / song database helpers --- */
constexpr size_t GetPVLoadParam   = 0x14040B2A0;    // Switch GetPVLoadParam
constexpr size_t GetGlobalPVInfo  = 0x1401D6520;    // Switch GetGlobalPVInfo
constexpr size_t pv_dbFindPVEntry = 0x1404BC500;    // Switch pv_db::FindPVEntry

/* --- Input --- */
constexpr size_t GetInputState            = 0x1402AC970; // Switch diva::GetInputState
constexpr size_t SetInputNotDispatchable  = 0x1402AC250; // Switch SetInputNotDispatchable
constexpr size_t PollInputRepeatAndDouble = 0x1402ABD90; // Switch per-state input poll hook
/* InputState internals (verified via xdis, MM+ 1.03):
 *   key bitsets (0xA2 keys, 64-bit words): tapped @0x00, down @0x30,
 *   repeat-tap @0x90, abs-tapped @0x19C8.  Positions: int32 @0xE0+i*4. */
constexpr size_t InputKeyCount      = 0xA2;
/* key bitsets, 3 u64 each (0xA2 keys); offsets verified by disassembling the
 * IsButton* accessors on MM+ 1.03 (see src/game/keys.h table):
 *   +0x00 tapped   (IsButtonTapped  fn 0x1402AAF80 via thunk 0x1402AB260)
 *   +0x18          (second tapped-related set, no public accessor found)
 *   +0x30 down     (IsButtonDown    fn 0x1402AAFE0 via thunk 0x1402AB280)
 *   +0x48 prev-down (read by PollInputRepeatAndDouble for edge computation)
 *   +0x60 rep-A    (written by PollInputRepeatAndDouble; accessor 0x1402AB010)
 *   +0x78 rep-B    (written by PollInputRepeatAndDouble; accessor 0x1402AB040,
 *                   called via thunk 0x1402AB290)
 *   +0x90 repeat   (IsButtonRepeat  fn 0x1402AB070 via thunk 0x1402AB270;
 *                   written by PollInputRepeatAndDouble)
 *   +0xA8          (unverified)
 *   +0xC0          (accessor 0x1402AB130)
 *   +0x19C8 abs-tapped (IsButtonTappedAbs 0x1402AB160)
 *   +0x1A28 input-blocked byte (nc diva.h IsInputBlocked) */
constexpr size_t InputOffTapped     = 0x00;
constexpr size_t InputOffTapped2    = 0x18;
constexpr size_t InputOffDown       = 0x30;
constexpr size_t InputOffPrevDown   = 0x48;
constexpr size_t InputOffRepA       = 0x60;
constexpr size_t InputOffRepB       = 0x78;
constexpr size_t InputOffRepeat     = 0x90;
constexpr size_t InputOffUnk0A8     = 0xA8;
constexpr size_t InputOffUnk0C0     = 0xC0;
constexpr size_t InputOffAbsTapped  = 0x19C8;
constexpr size_t InputOffPositions  = 0xE0;
constexpr size_t InputPosStickBase  = 0x14;   /* position index of L-stick X; L Y/R X/R Y follow */

/* --- PV arcade controller / key getters (AutoPlay) --- */
constexpr size_t PvArcadeCtrl        = 0x14026AFD0; // arcade ctrl: rcx = PVGameArcade*, xmm1 = dt
constexpr size_t GetPvKeyStateDown   = 0x140274930; // int64(handler, key): handler+0x68+key*8
constexpr size_t GetPvKeyStateTapped = 0x140274960; // int64(handler, key): handler+0xA8+key*8

/* --- Customize select (module picker) --- */
constexpr size_t CustomizeSelTaskInit = 0x140687D10; // Switch CustomizeSelTask init
constexpr size_t CustomizeSelTaskDest = 0x140687D80; // Switch CustomizeSelTask dest

} // namespace addr
