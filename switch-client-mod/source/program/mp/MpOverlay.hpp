#pragma once

namespace MpOverlay {
    /* Passive status chip, bottom-left. Renders only when
     * Config::mpShowStatus is on; never takes input. */
    void DrawWindow();
    bool IsVisible();
}
