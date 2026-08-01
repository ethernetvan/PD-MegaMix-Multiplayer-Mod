#pragma once
#include <string>
#include <vector>

class Config {
public:
    static bool enableDebug;
    static bool enableMultiplayer;
    static std::string serverIp;
    static int serverPort;
    static std::string modsDirectoryPath;
    static std::vector<std::string> priorityPaths;

    static bool init();
};