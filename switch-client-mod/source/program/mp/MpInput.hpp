#pragma once

#include <cstdint>
#include "../hid.hpp"

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

/* Call from keyboard.cpp ApplyInjections (end), all GetNpadState hooks.
 * When navOut is non-null it receives the state with inject applied but
 * BEFORE the block mask and stick zeroing (used only by the merged path). */
void Apply(nn::hid::NpadHandheldState* st,
           nn::hid::NpadHandheldState* navOut);

/* "A,B,Plus" -> bitmask (nn::hid::Button bits). */
uint64_t MaskFromNames(const char* names);

} // namespace mp::input
