#pragma once

#include <cstdint>
#include "../hid.hpp"

namespace mp::input {

/* Queue a tap: hold `buttons` for hold_ms, release for gap_ms, then next. */
void Tap(uint64_t buttons, uint32_t hold_ms = 50, uint32_t gap_ms = 50);

/* Repeated taps at hz for secs (hold = half period). secs==0 stops. */
void Spam(uint64_t buttons, uint32_t hz, uint32_t secs);
void StopSpam();

/* Real-input bits to suppress (Minus can never be blocked). */
void SetBlocked(uint64_t mask);
void ClearBlocked();

/* Zero the left analog stick (follower cursor lock). */
void SetZeroLStick(bool on);

/* Bypass: calls on the recorded thread see unmodified input
 * (used by our own GetMergedNpadState in nvnImguiCalc). */
void BeginBypass();
void EndBypass();

/* Clear every block/injection (emergency exit). */
void ResetAll();

/* Rising edges of REAL input (pre-block) — lets the lobby see a real
 * press even while the button is blocked for the game. */
uint64_t ConsumeRealPress(uint64_t mask);

/* Call from keyboard.cpp ApplyInjections (end), all GetNpadState hooks. */
void Apply(nn::hid::NpadHandheldState* st);

/* "A,B,Plus" -> bitmask (nn::hid::Button bits). */
uint64_t MaskFromNames(const char* names);

} // namespace mp::input
