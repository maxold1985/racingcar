#pragma once
#include <string>
#include <array>

struct RVGameConfig {
    std::string car;
    std::string script;
    std::string pavement;
    std::string character;
    std::string idle;
    std::string walking;
    std::string jump;
    std::string enterCar;
    std::array<std::string,32> buildingSlots{};
};

// Reads UTF-8 key=value text; relative paths are resolved from the config folder.
bool LoadGameConfig(const std::string& filename, RVGameConfig& out);
