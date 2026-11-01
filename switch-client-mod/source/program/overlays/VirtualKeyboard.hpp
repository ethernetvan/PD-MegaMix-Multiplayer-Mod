#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "imgui/imgui.h"

namespace VirtualKeyboard {
    bool IsOpen();
    bool IsVisible();
    /* true when the keyboard is open AND editing this buffer */
    bool IsOpenFor(const char* target_buf);

    int  GetCursorPos();

    void Open(char* target_buf, size_t buf_size, const char* submit_label = "SUBMIT", void (*on_submit_cb)() = nullptr);
    void Close();

    /* Pad navigation (used only by MpUi — SongQuickJump unaffected).
     * edges uses nn::hid::Button bits; moveX/moveY are -1/0/+1 steps. */
    void SetPadMode(bool on);
    void PadInput(uint64_t edges, int moveX, int moveY);

    bool ShouldFocusTarget();
    int  InputTextCallback(ImGuiInputTextCallbackData* data);

    void Draw();
}
