// PC port: mp_config.toml handling (toml++). Same [multiplayer] keys as the
// Switch config; file lives in the mod directory next to MegaMixMP.dll.
#include <windows.h>
#include "Config.hpp"
#include "platform/MpLog.hpp"
#include <tomlplusplus/toml.hpp>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace Config {

bool        mpEnabled       = true;
std::string mpServerMode    = "local";
std::string mpCloudUrl      = "mikumod.evanthomas.dev";
std::string mpServerIp      = "";
int32_t     mpServerPort    = 7777;
int32_t     mpDiscoveryPort = 7778;
std::string mpPlayerName    = "Player";
std::string mpNotifyStyle   = "avatars";
bool        mpRemoteAvatars = false;
bool        mpShowStatus    = false;
std::string mpClientId      = "";
std::string mpClientKey     = "";
bool        mpDebugCommands = false;

static std::wstring s_modDir;

void SetModDir(const wchar_t* dir) {
    s_modDir = dir ? dir : L"";
    if (!s_modDir.empty() && s_modDir.back() != L'\\') s_modDir += L'\\';
}
const std::wstring& ModDir() { return s_modDir; }

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::string ModDirA() { return WideToUtf8(s_modDir); }
std::string PathA(const char* name) { return ModDirA() + name; }

static std::string DefaultPlayerName() {
    wchar_t buf[64];
    DWORD n = sizeof(buf) / sizeof(buf[0]);
    if (GetUserNameW(buf, &n)) {
        std::string s = WideToUtf8(buf);
        if (s.size() > 16) s.resize(16);
        if (!s.empty()) return s;
    }
    return "Player";
}

/* 64 bits from the OS generator (MSVC's random_device is rand_s): the old
 * counter-seeded mt19937 could be guessed from the time the game started */
static std::string RandomHex64() {
    std::random_device rd;
    uint64_t v = ((uint64_t)rd() << 32) ^ (uint64_t)rd();
    char out[17];
    snprintf(out, sizeof(out), "%016llx", (unsigned long long)v);
    return out;
}
static std::string NewClientId() { return RandomHex64(); }

static std::wstring CfgPathW() { return s_modDir + L"mp_config.toml"; }

void Load() {
    std::wstring p = CfgPathW();
    FILE* f = nullptr;
    bool exists = (_wfopen_s(&f, p.c_str(), L"rb") == 0 && f);
    if (f) fclose(f);
    if (exists) {
        try {
            toml::table t = toml::parse_file(PathA("mp_config.toml"));
            auto mp = t["multiplayer"];
            mpEnabled       = mp["enabled"].value_or(true);
            mpServerMode    = mp["server_mode"].value_or("local");
            mpCloudUrl      = mp["cloud_url"].value_or("mikumod.evanthomas.dev");
            mpServerIp      = mp["server_ip"].value_or("");
            mpServerPort    = (int32_t)mp["server_port"].value_or<int64_t>(7777);
            mpDiscoveryPort = (int32_t)mp["discovery_port"].value_or<int64_t>(7778);
            mpPlayerName    = mp["player_name"].value_or("");
            mpNotifyStyle   = mp["notify_style"].value_or("avatars");
            mpRemoteAvatars = mp["remote_avatars"].value_or(false);
            mpShowStatus    = mp["debug_status_chip"].value_or(false);
            mpClientId      = mp["client_id"].value_or("");
            mpClientKey     = mp["client_key"].value_or("");
            mpDebugCommands = mp["debug_commands"].value_or(false);
        } catch (const std::exception& e) {
            WriteLog("[MP] mp_config.toml parse failed: %s\n", e.what());
        }
    }
    if (mpPlayerName.empty()) mpPlayerName = DefaultPlayerName();
    if (mpPlayerName.size() > 16) mpPlayerName.resize(16);
    bool dirty = !exists;
    if (mpClientId.size() != 16) { mpClientId = NewClientId(); dirty = true; }
    bool keyOk = mpClientKey.size() >= 32 && mpClientKey.size() <= 64;
    for (char ch : mpClientKey)
        keyOk = keyOk && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                          (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-');
    if (!keyOk) { mpClientKey = RandomHex64() + RandomHex64(); dirty = true; }
    if (mpNotifyStyle != "text" && mpNotifyStyle != "off")
        mpNotifyStyle = "avatars";
    if (mpServerMode != "cloud" && mpServerMode != "off")
        mpServerMode = "local";
    if (dirty) Save();
}

void RegenClientId() {
    mpClientId = NewClientId();
    Save();
}

void Save() {
    toml::table t;
    auto mp = toml::table{
        { "enabled",          mpEnabled },
        { "server_mode",      mpServerMode },
        { "cloud_url",        mpCloudUrl },
        { "server_ip",        mpServerIp },
        { "server_port",      (int64_t)mpServerPort },
        { "discovery_port",   (int64_t)mpDiscoveryPort },
        { "player_name",      mpPlayerName },
        { "notify_style",     mpNotifyStyle },
        { "remote_avatars",   mpRemoteAvatars },
        { "debug_status_chip", mpShowStatus },
        { "client_id",        mpClientId },
        { "client_key",       mpClientKey },
        { "debug_commands",   mpDebugCommands },
    };
    t.insert("multiplayer", mp);
    std::string path = PathA("mp_config.toml");
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) { WriteLog("[MP] can't write %s\n", path.c_str()); return; }
    os << t;
}

void Init() { Load(); }

} // namespace Config
