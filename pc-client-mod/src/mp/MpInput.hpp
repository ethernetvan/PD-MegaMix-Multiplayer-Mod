#pragma once

#include <cstdint>
#include "../platform/NxShim.hpp"

namespace diva { struct InputState; }

namespace mp::input {

/* Queue a tap: hold `buttons` for hold_ms, release for gap_ms, then next. */
/* force=true: inject reaches the game even while input is blocked (used by
 * the lobby's own pause/launch choreography). Admin/debug taps use the
 * default so the overlay block hides them from the game. */
void Tap(uint64_t buttons, uint32_t hold_ms = 50, uint32_t gap_ms = 50,
         bool force = false);

/* Repeated taps at hz for secs (hold = half period). secs==0 stops. */
void Spam(uint64_t buttons, uint32_t hz, uint32_t secs);
void StopSpam();

/* Real-input bits to suppress (Minus can never be blocked). */
void SetBlocked(uint64_t mask);
void ClearBlocked();

/* Zero both analog sticks (follower cursor lock / overlay open). */
void SetZeroSticks(bool on);

/* Bypass: calls on the recorded thread see unmodified input
 * (used by our own GetMergedNpadState in nvnImguiCalc). */
void BeginBypass();
void EndBypass();

/* Clear every block/injection (emergency exit). */
void ResetAll();

/* Rising edges of REAL input (pre-block) � lets the lobby see a real
 * press even while the button is blocked for the game. */
uint64_t ConsumeRealPress(uint64_t mask);

/* Pad navigation state: real + injected buttons BEFORE blocking, fed only
 * from the merged-state path in hid.hpp (GetMergedNpadState). Our UIs use
 * it to navigate while the game sees blocked input. */
void     NavUpdate(const nn::hid::NpadHandheldState& preBlock);
uint64_t NavConsume(uint64_t mask);   /* latched rising edges matching mask */
uint64_t NavHeld();                   /* currently held buttons */
void     NavStick(int* x, int* y);    /* left stick, signed */
void     NavClear();                  /* drop latched edges */

/* Call from the PollInputRepeatAndDouble hook (post-original), once per
 * InputState. Applies blocks/injections on the key bitsets; also feeds
 * real-edge latching, keylog and the menu model. */
void Apply(diva::InputState* st);

/* Per presented frame (mod.cpp): rebuilds the merged logical state,
 * updates nav, and checks the emergency exit (F9 / pad Back held 3 s). */
void PollMerged();

/* dbg_cmd keylog on|off: log every InputState key index that gets tapped. */
void SetKeylog(int on);

/* dbg_cmd rawtap {idx,hold}: inject a tap of raw key index <idx> into every
 * press bitset, bypassing the logical map — for index discovery. */
void RawTap(int32_t idx, uint32_t hold_ms);

/* dbg_cmd vktap {vk,hold}: force GetAsyncKeyState(vk) to report pressed for
 * hold_ms through the game's real keyboard pipeline (VK->index map ->
 * device bitset -> InputState). Works regardless of window focus. */
void VkTap(int vk, uint32_t hold_ms);

/* dbg_cmd watchp/watchoff: arm/clear a DR0 1-byte write watchpoint on an
 * address across all current threads; hits log RIP + stack return addrs. */
void WatchAddr(uintptr_t a);
void WatchOff();

/* dbg_cmd devtap {idx,hold}: hold raw device index <idx> in the keyboard
 * input object's post-map bitset (obj+0x518) for hold_ms. Produces real
 * edge/down state through the game's own pipeline — remap-proof. */
void DevTap(int32_t idx, uint32_t hold_ms);

/* Hook the keyboard poller (exe+0x2B0CF0) so DevTap bits land in the
 * +0x518 bitset after the original poll, before the caller's edge diff. */
void InstallKbPollHook();

/* Pause-menu task (TaskPauseMenuSwitch::Update hook): the real item cursor
 * at task+0x278 (0=Resume 1=Retry 2=Lag 3=Options 4=Return). -1 when the
 * menu is not up. Feeds mp::menu's guard/veto instead of blind counting. */
void InstallPauseTaskHook();
int  PauseMenuCursor();
void PauseMenuSetCursor(int c);
int  PauseMenuTaskState();
/* Native pause control (see MpInput.cpp "native pause control"). */
bool GamePaused();                         /* in a song and paused */
void PauseWant(int want, uint32_t hold_ms = 0);  /* 1 pause, 0 run, -1 off */
void PauseWantRelease();                   /* drop a persistent target */
void SetBackgroundPause(bool gameDefault); /* false = don't pause unfocused */
/* PVGameLoaderFinishUp hook (exe+0x262730): the MP start gate. */
void InstallLoaderHook();

/* dbg_cmd tracetap {on}: log every IsButtonTapped(nav key) call site —
 * finds which code consumes menu navigation (menu-dispatch RE aid). */
void SetTraceTap(int on);
void InstallTappedHook();

/* Install the DirectInput8 GetDeviceState/GetDeviceData hooks that give
 * injected logical buttons a real-DIK signature (PC scene input path). */
void InstallDiHooks();

/* Hook the game's copied GetAsyncKeyState pointer (exe+0xB257F8) so logical
 * taps also produce real VK presses through the keyboard device pipeline. */
void InstallGaksHook();
int  KeylogOn();
void KLog(const char* fmt, ...);

/* "A,B,Plus" -> bitmask (nn::hid::Button bits). */
uint64_t MaskFromNames(const char* names);

} // namespace mp::input
