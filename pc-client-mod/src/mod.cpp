// MegaMixMP — DivaModLoader mod entry point (PC port of the Switch
// multiplayer client). D3D11/Win32 ImGui via DML callbacks; game hooks live
// in game/MpSelPc.cpp.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "platform/MpLog.hpp"
#include "platform/NxShim.hpp"
#include "platform/MpSock.hpp"
#include "Config.hpp"
#include "MpFont.hpp"
#include "game/hooks.h"
#include "game/addresses.h"
#include "mp/MpNet.hpp"
#include "mp/MpLobby.hpp"
#include "mp/MpUi.hpp"
#include "mp/MpInput.hpp"
#include "mp/MpSpr.hpp"
#include "mp/MpAvatar.hpp"
#include "game/MpAutoPlay.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void InstallMpGameHooks();

namespace ImGui {
/* Switch-compat: the overlay grabs input focus; dbg cursor injection for the
 * admin `cursor` command. */
bool g_imguiHasFocus = false;
static float s_dbgX = -1.0f, s_dbgY = -1.0f;
static bool  s_dbgClick = false, s_dbgPending = false;
void DbgSetCursor(float x, float y, bool click) {
    s_dbgX = x; s_dbgY = y; s_dbgClick = click; s_dbgPending = true;
}
} // namespace ImGui

static HMODULE  s_self = nullptr;
static ID3D11Device*        s_dev = nullptr;
static ID3D11DeviceContext* s_ctx = nullptr;
static HWND     s_hwnd = nullptr;
static WNDPROC  s_origWndProc = nullptr;
static bool     s_imguiReady = false;
static float    s_lastBufH = 0.0f;

/* ---------------------------------------------------------------- */
/* WndProc hook: ImGui gets input only while the MP panel is open.     */
/* ---------------------------------------------------------------- */
static bool PanelSwallowsInput(UINT msg) {
    if (!mp::lobby::OverlayOpen()) return false;
    ImGuiIO& io = ImGui::GetIO();
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL: case WM_MOUSEMOVE:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        return io.WantCaptureMouse;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_CHAR: case WM_SETCURSOR:
        return io.WantCaptureKeyboard || msg == WM_CHAR;
    }
    return false;
}

static LRESULT CALLBACK MpWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (mp::input::KeylogOn() &&
        (msg == WM_KEYDOWN || msg == WM_KEYUP ||
         msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        const char* n = msg == WM_KEYDOWN ? "wm_keydown" :
                        msg == WM_KEYUP ? "wm_keyup" :
                        msg == WM_SYSKEYDOWN ? "wm_syskeydown" : "wm_syskeyup";
        mp::input::KLog("[MP] keylog t=%llu %s vk=%d scan=%d\n",
                 (unsigned long long)GetTickCount64(), n, (int)wParam,
                 (int)((lParam >> 16) & 0xFF));
    }
    if (s_imguiReady) {
        /* F8 / Esc are polled in PanelHotkeys(); swallow the messages so
         * the panel toggle never runs twice */
        if ((msg == WM_KEYDOWN || msg == WM_KEYUP) &&
            (wParam == VK_F8 ||
             (wParam == VK_ESCAPE && mp::lobby::OverlayOpen())))
            return 0;
        if (PanelSwallowsInput(msg)) {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
            return 0;
        }
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }
    return CallWindowProcW(s_origWndProc, hWnd, msg, wParam, lParam);
}

/* F8 toggles the panel, Esc closes it. Polled once per frame and only
 * while the game window is foreground, so it works whether or not the
 * game's window receives keyboard messages. */
static void PanelHotkeys() {
    static bool s_f8 = false, s_esc = false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool focused = fg && fg != GetConsoleWindow() &&
                   pid == GetCurrentProcessId();
    bool f8 = focused && (GetAsyncKeyState(VK_F8) & 0x8000);
    bool esc = focused && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
    if (f8 && !s_f8) {
        mp::lobby::SetOverlayOpen(!mp::lobby::OverlayOpen());
        WriteLog("[MP] F8: panel %s\n", mp::lobby::OverlayOpen() ? "open" : "closed");
    }
    if (esc && !s_esc && mp::lobby::OverlayOpen())
        mp::lobby::SetOverlayOpen(false);
    s_f8 = f8; s_esc = esc;
}

/* ---------------------------------------------------------------- */
/* D3D11                                                             */
/* ---------------------------------------------------------------- */

static void MpImGuiInit(HWND hwnd) {
    if (s_imguiReady) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding  = 6.0f;
    st.GrabRounding   = 6.0f;
    st.PopupRounding  = 6.0f;
    st.Colors[ImGuiCol_WindowBg]     = ImVec4(0.106f, 0.118f, 0.137f, 0.94f);
    st.Colors[ImGuiCol_FrameBg]      = ImVec4(0.149f, 0.165f, 0.192f, 1.0f);
    st.Colors[ImGuiCol_Button]       = ImVec4(0.224f, 0.773f, 0.733f, 0.28f);
    st.Colors[ImGuiCol_ButtonHovered]= ImVec4(0.224f, 0.773f, 0.733f, 0.45f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.224f, 0.773f, 0.733f, 0.65f);
    st.Colors[ImGuiCol_CheckMark]    = ImVec4(0.224f, 0.773f, 0.733f, 1.0f);
    st.Colors[ImGuiCol_Header]       = ImVec4(0.224f, 0.773f, 0.733f, 0.20f);
    st.Colors[ImGuiCol_HeaderHovered]= ImVec4(0.224f, 0.773f, 0.733f, 0.35f);
    st.Colors[ImGuiCol_Tab]          = ImVec4(0.149f, 0.165f, 0.192f, 1.0f);
    st.Colors[ImGuiCol_TabHovered]   = ImVec4(0.224f, 0.773f, 0.733f, 0.45f);
    st.Colors[ImGuiCol_TabActive]    = ImVec4(0.224f, 0.773f, 0.733f, 0.40f);
    st.Colors[ImGuiCol_NavHighlight] = ImVec4(0.224f, 0.773f, 0.733f, 0.80f);
    st.Colors[ImGuiCol_TitleBg]          = ImVec4(0.106f, 0.118f, 0.137f, 1.0f);
    st.Colors[ImGuiCol_TitleBgActive]    = ImVec4(0.118f, 0.157f, 0.165f, 1.0f);
    st.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.106f, 0.118f, 0.137f, 0.75f);
    st.Colors[ImGuiCol_ResizeGrip]        = ImVec4(0.224f, 0.773f, 0.733f, 0.20f);
    st.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.224f, 0.773f, 0.733f, 0.45f);
    st.Colors[ImGuiCol_ResizeGripActive]  = ImVec4(0.224f, 0.773f, 0.733f, 0.65f);
    st.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.224f, 0.773f, 0.733f, 0.45f);
    st.Colors[ImGuiCol_SliderGrab]       = ImVec4(0.224f, 0.773f, 0.733f, 0.45f);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.224f, 0.773f, 0.733f, 0.65f);
    st.Colors[ImGuiCol_Border]           = ImVec4(0.243f, 0.271f, 0.310f, 0.60f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(s_dev, s_ctx);

    s_imguiReady = true;
    WriteLog("[MP] ImGui initialized (dx11+win32)\n");
}

/* scale of the game's letterboxed 16:9 HUD for this backbuffer */
static float HudScale(const DXGI_SWAP_CHAIN_DESC& d) {
    float sx = d.BufferDesc.Width / 1280.0f, sy = d.BufferDesc.Height / 720.0f;
    return sx < sy ? sx : sy;
}

static void MpNewFrame(IDXGISwapChain* swap) {
    DXGI_SWAP_CHAIN_DESC desc;
    swap->GetDesc(&desc);
    HWND hwnd = desc.OutputWindow;
    if (hwnd != s_hwnd) {
        if (s_origWndProc)
            SetWindowLongPtrW(s_hwnd, GWLP_WNDPROC, (LONG_PTR)s_origWndProc);
        s_hwnd = hwnd;
        s_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                 (LONG_PTR)MpWndProc);
    }
    float bh = (float)desc.BufferDesc.Height;
    if (bh != s_lastBufH || !s_imguiReady) {
        s_lastBufH = bh;
        if (!s_imguiReady) MpImGuiInit(hwnd);
        else if (s_imguiReady) {
            /* rebuild fonts for the new scale */
            FontLoader::LoadFonts(ImGui::GetIO(), Config::ModDirA().c_str(),
                                  HudScale(desc));
            ImGui_ImplDX11_InvalidateDeviceObjects();
            ImGui_ImplDX11_CreateDeviceObjects();
        }
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = mp::lobby::OverlayOpen();

    /* dbg cursor injection (admin `cursor` cmd) */
    if (ImGui::s_dbgPending) {
        ImGui::s_dbgPending = false;
        float s = io.DisplaySize.y / 720.0f;
        io.AddMousePosEvent(ImGui::s_dbgX * s, ImGui::s_dbgY * s);
        io.AddMouseButtonEvent(0, ImGui::s_dbgClick);
    }
}

static void MpDrawAll(IDXGISwapChain* swap) {
    (void)swap;
    /* MpOverlay status chip + song-select chip */
    extern void MpStatusChipDraw();
    MpStatusChipDraw();

    /* parity order — ImGuiOverlay.cpp draw block */
    mp::lobby::Snapshot sn;
    mp::lobby::GetSnapshot(&sn);
    int ph = sn.phase;
    mp::lobby::DrawPanel();

    /* The game draws its 2D HUD in a 16:9 rect letterboxed into the window
     * (fit to the smaller scale, centred): a 1910x1175 client gets a
     * 1910x1074 HUD 50 px from the top. The Switch HUD code works in 720p
     * units x DisplaySize.y/720 on a full 16:9 screen, so give it a
     * DisplaySize equal to the HUD rect and then move everything it drew
     * into place. Measured on the live game: the clear gauge frame sits at
     * x 194..1836, y 1085.. of that client, which is Switch 130..1230,
     * 692.. under this mapping. */
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 real = io.DisplaySize;
    const float hs = (real.x / 1280.0f < real.y / 720.0f) ? real.x / 1280.0f
                                                           : real.y / 720.0f;
    const ImVec2 hud(1280.0f * hs, 720.0f * hs);
    const ImVec2 off((real.x - hud.x) * 0.5f, (real.y - hud.y) * 0.5f);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    bg->AddDrawCmd(); fg->AddDrawCmd();      /* HUD gets its own commands */
    const int bgV = bg->VtxBuffer.Size, fgV = fg->VtxBuffer.Size;
    const int bgC = bg->CmdBuffer.Size - 1, fgC = fg->CmdBuffer.Size - 1;
    mp::lobby::HudCaptureReset();
    io.DisplaySize = hud;

    if ((ph == 3 || ph == 4 || ph == 5) && !sn.inSongSelect && sn.resultsAgeMs < 0)
        mp::lobby::DrawLeaderboard();
    if (sn.inSong) {
        mp::lobby::DrawRankGlow();
        mp::lobby::DrawGaugeRace();
    }
    AutoPlay::DrawIndicator();
    mp::lobby::DrawPopups();
    mp::lobby::DrawCountdown();
    if (sn.waitingStart && sn.countdownMs < 0) mp::lobby::DrawWaiting();
    if (sn.resultsAgeMs >= 0 && sn.standingCount > 0)
        mp::lobby::DrawResultsAvatars();
    else if ((ph == 6 || ph == 5) && !sn.inSongSelect)
        mp::lobby::DrawStandings();
    mp::lobby::DrawBanners();
    mp::lobby::DrawEmergency();

    io.DisplaySize = real;
    if (off.x != 0.0f || off.y != 0.0f) {
        auto move = [&](ImDrawList* dl, int v0, int c0) {
            for (int i = v0; i < dl->VtxBuffer.Size; i++) {
                dl->VtxBuffer[i].pos.x += off.x;
                dl->VtxBuffer[i].pos.y += off.y;
            }
            for (int i = c0 < 0 ? 0 : c0; i < dl->CmdBuffer.Size; i++) {
                ImVec4& r = dl->CmdBuffer[i].ClipRect;
                r.x += off.x; r.y += off.y; r.z += off.x; r.w += off.y;
            }
        };
        move(bg, bgV, bgC);
        move(fg, fgV, fgC);
        ImDrawList* wins[16];
        int n = mp::lobby::HudCaptured(wins, 16);
        for (int i = 0; i < n; i++) move(wins[i], 0, 0);
    }
}

/* ---------------------------------------------------------------- */
/* DML exports                                                       */
/* ---------------------------------------------------------------- */
static std::wstring ModDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(s_self, buf, MAX_PATH);
    std::wstring p = buf;
    size_t sl = p.find_last_of(L"\\/");
    return sl != std::wstring::npos ? p.substr(0, sl + 1) : p;
}

extern "C" {

__declspec(dllexport) void PreInit() {}

__declspec(dllexport) void Init() {
    s_self = nullptr;
    /* our HMODULE: read it from a static function address */
    HMODULE hm = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Init, &hm);
    s_self = hm;

    /* redirect stdout only when a console already exists (DML config
     * console = true creates one); never force-allocate our own */
    if (GetConsoleWindow() != nullptr) {
        FILE* dummy;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        /* a click on a QuickEdit console pauses every writer to it; turn
         * QuickEdit off so the dev console can't stall the process */
        HANDLE hin = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (hin != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hin, &mode))
                SetConsoleMode(hin, (mode & ~(DWORD)ENABLE_QUICK_EDIT_MODE) |
                                        ENABLE_EXTENDED_FLAGS);
            CloseHandle(hin);
        }
    }

    std::wstring dir = ModDir();
    MpLogInit(dir.c_str());
    Config::SetModDir(dir.c_str());
    nn::fs::FsSetModDir(dir.c_str());
    Config::Load();

    WriteLog("[MP] MegaMixMP init, moddir=%ls\n", dir.c_str());
    WriteLog("[MP] player='%s' cid=%s mode=%s\n",
             Config::mpPlayerName.c_str(), Config::mpClientId.c_str(),
             Config::mpServerMode.c_str());

    mp::ApiResolveAll();
    InstallMpGameHooks();
    /* DI8 (DIK scancode) and GAKS (VK) injection are disabled — both depend
     * on physical key identity, and the GAKS hook also leaks the user's
     * real background-app keystrokes into the game. Injection runs through
     * the post-map device bitset (DevTap) + abstract InputState indices. */
    mp::input::InstallPauseTaskHook();
    mp::input::InstallLoaderHook();
    mp::input::InstallKbPollHook();   /* focus gate for the keyboard */
    mp::spr::Install();               /* put_sprite hook: race fill under markers */
#ifdef MP_DEV_TOOLS
    mp::input::InstallTappedHook();
    mp::input::InstallDiHooks();
    mp::input::InstallGaksHook();
#endif

    if (Config::mpEnabled)
        mp::Init();
    else
        WriteLog("[MP] disabled by config\n");
}

__declspec(dllexport) void PostInit() {}

__declspec(dllexport) void D3DInit(IDXGISwapChain* swap, ID3D11Device* dev,
                                   ID3D11DeviceContext* ctx) {
    s_dev = dev;
    s_ctx = ctx;
    mp::avatar::SetDevice(dev, ctx);
    DXGI_SWAP_CHAIN_DESC desc;
    swap->GetDesc(&desc);
    /* s_hwnd stays unset: MpNewFrame subclasses the window on the first
     * frame (setting it here made that check skip the subclass, so the
     * WndProc hook - mouse/typing for the panel - was never installed) */
    s_lastBufH = (float)desc.BufferDesc.Height;
    MpImGuiInit(desc.OutputWindow);
    FontLoader::LoadFonts(ImGui::GetIO(), Config::ModDirA().c_str(),
                          HudScale(desc));
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui_ImplDX11_CreateDeviceObjects();
    WriteLog("[MP] D3DInit %ux%u\n", desc.BufferDesc.Width, desc.BufferDesc.Height);
}

__declspec(dllexport) void OnFrame(IDXGISwapChain* swap) {
    if (!Config::mpEnabled) return;
    mp::OnFrame();
    /* in a room the song must keep running while the window is unfocused */
    mp::input::SetBackgroundPause(!mp::lobby::InLobby());
    mp::input::PollMerged();
    if (!s_imguiReady) return;
    PanelHotkeys();
    mp::avatar::RenderPump();   /* GPU readback/decode on the present thread */
    MpNewFrame(swap);
    MpDrawAll(swap);
    ImGui::Render();
    /* DML runs its real ResizeBuffers before OnResize: never hold a
     * backbuffer reference across frames or ResizeBuffers fails */
    ID3D11Texture2D* bb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
        s_dev->CreateRenderTargetView(bb, nullptr, &rtv);
        bb->Release();
    }
    if (rtv) {
        s_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        rtv->Release();
    }
}

__declspec(dllexport) void OnResize(IDXGISwapChain* swap) {
    if (s_imguiReady) {
        DXGI_SWAP_CHAIN_DESC desc;
        swap->GetDesc(&desc);
        s_lastBufH = (float)desc.BufferDesc.Height;
        FontLoader::LoadFonts(ImGui::GetIO(), Config::ModDirA().c_str(),
                          HudScale(desc));
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
    }
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) s_self = mod;
    return TRUE;
}
