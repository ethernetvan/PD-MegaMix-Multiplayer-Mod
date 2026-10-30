#pragma once
#include <cstdint>



namespace Multiplayer {
    void Init();
    void Draw();
    void OnGameStateStart(int32_t pvId, void* scorePtr);
    void SetSongStarted(bool started);
}
