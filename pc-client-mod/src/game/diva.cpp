// PC port: InputState methods (nc input.cpp, MIT — see
// third_party/LICENSE.newclassics.txt). Verified bitset/position offsets via
// xdis on MM+ 1.03 (see addresses.h).
#include "diva.h"

namespace diva {

constexpr int32_t MaxKeyCount = 0xA2;

float InputState::GetPosition(int32_t index)
{
	if (index < 0x18)
		return (float)Positions()[index];
	return 0.0f;
}

int32_t InputState::GetDevice()
{
	return *reinterpret_cast<int32_t*>(&_data[0x1A24]);
}

bool InputState::IsButtonDown(int32_t key)
{
	if (key >= 0 && key < MaxKeyCount)
		return (KeysDown()[key >> 6] >> (key & 0x3F)) & 1;
	return false;
}

bool InputState::IsButtonTapped(int32_t key)
{
	if (key >= 0 && key < MaxKeyCount)
		return (KeysTapped()[key >> 6] >> (key & 0x3F)) & 1;
	return false;
}

bool InputState::IsButtonTappedAbs(int32_t key)
{
	if (key >= 0 && key < MaxKeyCount)
		return (KeysAbsTapped()[key >> 6] >> (key & 0x3F)) & 1;
	return false;
}

bool InputState::IsButtonTappedOrRepeat(int32_t key)
{
	if (key >= 0 && key < MaxKeyCount)
		return (KeysRepeat()[key >> 6] >> (key & 0x3F)) & 1;
	return false;
}

} // namespace diva

diva::PVGameArcade* g_current_game = nullptr;
