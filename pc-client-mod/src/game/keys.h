#pragma once
// DivaMegaMix InputState key indices -> logical nn::hid buttons.
// Indices 3-12 come from nc ui/common.cpp key handling (MM+ 1.03).
// Indices discovered via the `keylog` dbg_cmd at runtime are marked
// `// keylog:<how verified>`; unverified mappings are marked TODO-H2.
#include <cstdint>

namespace keys {

/* Verified from nc ui/common.cpp (uses these indices directly). */
constexpr int32_t Up      = 3;   // arrow up / nav up
constexpr int32_t Down    = 4;   // arrow down / nav down
constexpr int32_t Left    = 5;   // arrow left
constexpr int32_t Right   = 6;   // arrow right
constexpr int32_t Preview = 7;   // preview (Switch X-ish)
constexpr int32_t Cancel  = 9;   // cancel / back (Switch B)
constexpr int32_t Enter   = 10;  // confirm (Switch A)
constexpr int32_t SwapL   = 11;  // swap left (Switch L)
constexpr int32_t SwapR   = 12;  // swap right (Switch R)
constexpr int32_t F7      = 13;  // nc: F7 / L2
constexpr int32_t L2      = 92;  // nc: alternate L2

/* PC input-path findings (verified by real keypress 2026-11-17, MM+ 1.03,
 * user-pressed physical keys with `keylog on`, game focused on DISPLAY2):
 * - Keyboard input does NOT come from WndProc WM_KEY* or DirectInput —
 *   the keyboard device update (0x1402aee40) walks a VK->index std::map
 *   (fn 0x1402b0cf0, head ptr at exe+0x114AB930) calling GetAsyncKeyState
 *   per VK through a copied import pointer (exe+0xB257F8) and writes a
 *   device bitset at device+0x518; the InputState merge derives the rest.
 *   No di_state/di_data/wm_* lines appear for any real key.
 * - Per press, bitsets: tap@+0x00 (press edge), down@+0x30 (held),
 *   prev@+0x48 (held, 1 frame late), rep@+0x90 (1-frame pulse on press),
 *   u0a8@+0xA8 (held), abs@+0x19C8 (1-frame pulse, 1 frame late);
 *   tap2@+0x18 fires on RELEASE. repA/repB/u0c0 unused for keyboard.
 * - Signatures (press -> InputState indices, slot 0):
 *     Enter {10,55,161}   Up {3,123}   Down {4,128}   Left {5,125}
 *     Right {6,126}       Bksp {41}    Space {84}
 *     F1 {17,86}  F2 {87,160}  F3 {15,88}  F4 {16,89}  F7 {92}  F9 {94}
 *     Esc gameplay {28,160}; Esc in pause menu {9,28,161}
 *     mouse click (focus) {140,144,145}
 *   Raw index = scancode+27 (Enter 0x1C->55, Esc 0x01->28, Space 0x39->84,
 *   Bksp 0x0E->41, F1..F4 0x3B..->86..89, F7 0x41->92, F9 0x43->94;
 *   arrows are extended scancodes -> 123/125/126/128).
 *   The 9/160/161/abstract aliases are context-dependent — the merge adds
 *   index 9 (Cancel) for Esc only when a menu is active.
 * - Pause = Esc on PC. The pause check reads the DEVICE layer, not
 *   InputState: the keyboard poller 0x1402B0CF0 walks the VK->index
 *   std::map (head at exe+0x114AB930, node{_Left0,_Parent8,_Right0x10,
 *   vk@0x1C, idx@0x20}) and sets index bits in the input object's bitset
 *   at +0x518. Callers diff old-vs-new +0x518 for edges.
 *   Verified 2026-11-18 (unfocused game, DR write-watch on pv->paused):
 *   the per-frame pv update's pause branch writes pv_game->paused at
 *   0x1402425b6 (rbx=PVGameData=0x1412C2330), triggered by the device
 *   bitset. OR-ing index 28 into +0x518 after the poll (input::DevTap)
 *   pauses AND resumes cleanly — no OS input, no physical-key identity.
 *   Writing InputState index 28/9/161 alone does NOT pause (proven).
 * - Full VK->device-index map dumped from live process (102 entries,
 *   2026-11-18): VK 0x1B Esc->28, 0x0D Enter->55, 0x08 Bksp->41,
 *   0x20 Space->84, arrows 0x25-0x28 ->125/123/126/128, F1-F12
 *   0x70-0x7B ->86-97, digits/letters/numpad/OEM per table. */
constexpr int32_t RawEnter = 55;  // Enter raw
constexpr int32_t RawEsc   = 28;  // Esc raw = pause key index
constexpr int32_t RawSpace = 84;
constexpr int32_t RawBksp  = 41;
constexpr int32_t RawUp    = 123;
constexpr int32_t RawDown  = 128;
constexpr int32_t RawLeft  = 125;
constexpr int32_t RawRight = 126;
constexpr int32_t AnyKey   = 161; // keyboard any-key (never map to logical)
/* Abstract pause action: the pause manager (exe+0x65BE90) and the pause
 * menu (exe+0x65A056) both test IsButtonTapped(0xA0). Whatever key/button
 * the user bound to pause lands here, so a real pause press is detected
 * (and blocked in guarded rooms) at this index. The mod itself never
 * injects it: pausing/resuming goes through the native pause control in
 * MpInput.cpp. */
constexpr int32_t PauseAction = 0xA0;
constexpr int32_t Plus    = PauseAction;
constexpr int32_t Minus   = -1;  // intentionally unmapped (never blocked)

/* logical nn::hid::Button bit -> InputState key index. -1 = unmapped. */
inline int32_t KeyForLogical(uint64_t bit) {
    switch (bit) {
    case (1ull << 0):  return Enter;    /* A     */
    case (1ull << 1):  return Cancel;   /* B     */
    case (1ull << 2):  return Preview;  /* X     */
    case (1ull << 3):  return F7;       /* Y (TODO-H2 verify) */
    case (1ull << 6):  return SwapL;    /* L     */
    case (1ull << 7):  return SwapR;    /* R     */
    case (1ull << 10): return Plus;     /* Plus  */
    case (1ull << 11): return Minus;    /* Minus */
    case (1ull << 13): return Up;       /* Up    */
    case (1ull << 15): return Down;     /* Down  */
    case (1ull << 12): return Left;     /* Left  */
    case (1ull << 14): return Right;    /* Right */
    default:           return -1;
    }
}

/* Full injection signature per logical button: every InputState index the
 * corresponding physical key sets (abstract action + raw-key + any-key
 * indices) — verified by real keypress 2026-11-17 (see table above). */
inline int32_t KeySignature(uint64_t bit, int32_t* out, int32_t maxOut) {
    int n = 0;
    auto put = [&](int32_t k) { if (n < maxOut) out[n++] = k; };
    int32_t a = KeyForLogical(bit);
    if (a < 0) return 0;
    put(a);
    switch (bit) {
    case (1ull << 0):                 /* A / Enter */
        put(RawEnter); put(AnyKey);
        break;
    case (1ull << 1):                 /* B / Cancel = Esc signature */
        put(RawEsc); put(AnyKey);
        break;
    case (1ull << 3):                 /* Y / F7 */
        put(L2); /* L2 (92) is F7's raw index */
        break;
    case (1ull << 13): put(RawUp);    break;
    case (1ull << 15): put(RawDown);  break;
    case (1ull << 12): put(RawLeft);  break;
    case (1ull << 14): put(RawRight); break;
    default: break;
    }
    return n;
}

/* InputState key index -> logical button bit (0 if unmapped). Raw-key
 * indices that belong to exactly one physical key also map here so real
 * keyboard presses feed the menu model/nav like Switch Npad buttons did.
 * The any-key index (161) must NOT map: every key sets it. */
inline uint64_t LogicalForKey(int32_t key) {
    switch (key) {
    case Enter:   return 1ull << 0;   /* A     */
    case Cancel:  return 1ull << 1;   /* B     */
    case Preview: return 1ull << 2;   /* X     */
    case F7:      return 1ull << 3;   /* Y     */
    case SwapL:   return 1ull << 6;   /* L     */
    case SwapR:   return 1ull << 7;   /* R     */
    case Up:      return 1ull << 13;  /* Up    */
    case Down:    return 1ull << 15;  /* Down  */
    case Left:    return 1ull << 12;  /* Left  */
    case Right:   return 1ull << 14;  /* Right */
    case Plus:    return 1ull << 10;  /* Plus  */
    /* raw-key indices map to the same logical bits so real keyboard
     * presses feed nav/menu like the abstract action indices do */
    case RawEnter:  return 1ull << 0;
    case RawUp:     return 1ull << 13;
    case RawDown:   return 1ull << 15;
    case RawLeft:   return 1ull << 12;
    case RawRight:  return 1ull << 14;
    case L2:        return 1ull << 3;  /* raw F7 */
    default:      return 0;
    }
}

} // namespace keys
