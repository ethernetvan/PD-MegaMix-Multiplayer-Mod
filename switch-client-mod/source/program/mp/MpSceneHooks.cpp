/* MP-only scene hooks for builds WITHOUT New Classics: same pv_sel/results
 * trampolines as nc/ui/pv_sel_switch.cpp + result_switch.cpp, minus the
 * NC-asset/style code. Installed only when mpEnabled && !enableNewClassics
 * && !(enableFtUi || forceFtUI). */
#include "../nc/ui/pv_sel.hpp"
#include "../nc/ui/common.hpp"
#include "../nc/nc_state.hpp"
#include "../nc/util.hpp"
#include "../overlays/Hints.hpp"
#include "../overlays/SongQuickJump.hpp"
#include "MpNet.hpp"
#include "MpMenu.hpp"
#include "MpSceneHooks.hpp"
#include "lib.hpp"
#include <nn.hpp>

static inline FUNCTION_PTR(bool, __fastcall, IsFadeEnd, 0x41a0b0);

HOOK_DEFINE_TRAMPOLINE(MpPvSelInitHook) {
    static bool Callback(uint64_t a1) {
        Hints::SetPvSelActive(false);
        Hints::SetSongConfirmActive(false);
        SongQuickJump::SetPvSelActive(false);
        mp::OnPvSelInit();
        return Orig(a1);
    }
};

HOOK_DEFINE_TRAMPOLINE(MpPvSelCtrlHook) {
    static bool Callback(void* sel) {
        bool ret = Orig(sel);
        int32_t st = *reinterpret_cast<int32_t*>(
                         reinterpret_cast<uintptr_t>(sel) + 0x68);
        if (st == 6) {
            uint8_t is_busy = *reinterpret_cast<uint8_t*>(
                              reinterpret_cast<uintptr_t>(sel) + 0x2587C);
            if (is_busy == 0 && IsFadeEnd()) {
                Hints::ProcessInput();
                Hints::SetPvSelActive(true);
                Hints::SetSongConfirmActive(false);
                SongQuickJump::SetPvSelActive(true, sel);
            } else {
                Hints::SetPvSelActive(false);
                Hints::SetSongConfirmActive(false);
                SongQuickJump::SetPvSelActive(false);
            }
        } else if (st == 8) {
            Hints::ProcessInput();
            Hints::SetPvSelActive(false);
            Hints::SetSongConfirmActive(true);
            SongQuickJump::SetPvSelActive(false);
        } else {
            Hints::SetPvSelActive(false);
            Hints::SetSongConfirmActive(false);
            SongQuickJump::SetPvSelActive(false);
        }
        mp::OnPvSelFrame(sel);
        return ret;
    }
};

HOOK_DEFINE_TRAMPOLINE(MpPvSelDestHook) {
    static bool Callback(uint64_t a1) {
        mp::OnPvSelDest();
        Hints::SetPvSelActive(false);
        Hints::SetSongConfirmActive(false);
        SongQuickJump::SetPvSelActive(false);
        return Orig(a1);
    }
};

HOOK_DEFINE_TRAMPOLINE(MpStageResultInitHook) {
    static bool Callback(uint64_t a1) {
        mp::OnStageResult(true);
        return Orig(a1);
    }
};

HOOK_DEFINE_TRAMPOLINE(MpStageResultDestHook) {
    static bool Callback(uint64_t a1) {
        mp::OnStageResult(false);
        return Orig(a1);
    }
};

HOOK_DEFINE_TRAMPOLINE(MpAetInLoopOutSetLayerHook) {
    static void Callback(void* a1, const void* in, const void* loop,
                         const void* out, int32_t prio) {
        const libcxx_string* in_str =
            reinterpret_cast<const libcxx_string*>(in);
        if (in_str && util::StartsWith(in_str->c_str(), "win_vp_in")) {
            uint64_t ms = (uint64_t)nn::os::GetSystemTick().GetInt64Value()
                * 1000 / nn::os::GetSystemTickFrequency();
            mp::menu::ResultsVpShown(ms);
        }
        Orig(a1, in, loop, out, prio);
    }
};

void mp::scenehooks::Install() {
    MpPvSelInitHook::InstallAtOffset(0x882c10);
    MpPvSelCtrlHook::InstallAtOffset(0x883af0);
    MpPvSelDestHook::InstallAtOffset(0x8860f0);
    MpStageResultInitHook::InstallAtOffset(0x6dd1c0);
    MpStageResultDestHook::InstallAtOffset(0x6ddac0);
    MpAetInLoopOutSetLayerHook::InstallAtOffset(0x71b120);
}
