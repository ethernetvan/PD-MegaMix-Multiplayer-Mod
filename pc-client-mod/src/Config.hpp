#pragma once
// PC port: mp_config.toml in the mod directory. Mirrors the Switch
// [multiplayer] table keys (same names/types the Switch Config exposes).
#include <cstdint>
#include <string>
#include <vector>

namespace Config {

extern bool        mpEnabled;         /* "enabled" */
extern std::string mpServerMode;      /* "server_mode": "local"|"cloud"|"off" */
extern std::string mpCloudUrl;        /* "cloud_url" */
extern std::string mpServerIp;        /* "server_ip" */
extern int32_t     mpServerPort;      /* "server_port" */
extern int32_t     mpDiscoveryPort;   /* "discovery_port" */
extern std::string mpPlayerName;      /* "player_name" */
extern std::string mpNotifyStyle;     /* "notify_style": avatars|text|off */
extern bool        mpRemoteAvatars;   /* "remote_avatars" */
extern bool        mpShowStatus;      /* "debug_status_chip" */
extern std::string mpClientId;        /* "client_id" (16 hex, generated once) */
extern std::string mpClientKey;       /* "client_key": secret that proves client_id is ours */
extern bool        mpDebugCommands;   /* "debug_commands": accept the server's dbg_cmd */

void RegenClientId();                 /* new client_id + save (id clash on a server) */

/* mod directory (where the DLL lives); set once by mod.cpp before Load. */
void SetModDir(const wchar_t* dir);
const std::wstring& ModDir();
std::string ModDirA();
std::string PathA(const char* name);    /* <moddir>\<name> as UTF-8 */

void Load();
void Save();
void Init();

} // namespace Config
