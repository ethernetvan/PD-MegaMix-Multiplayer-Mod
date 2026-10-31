#pragma once

#include <cstddef>
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

    bool ShouldFocusTarget();
    int  InputTextCallback(ImGuiInputTextCallbackData* data);

    void Draw();
}
