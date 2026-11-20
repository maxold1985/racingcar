#pragma once
#include <string>
#include <array>
#include <limits>

struct RVGameConfig {
    std::string car;
    std::string script;
    std::string pavement;
    std::string roadsShp;
    float roadWidth=8.0f;
    double shpOriginX=std::numeric_limits<double>::quiet_NaN();
    double shpOriginY=std::numeric_limits<double>::quiet_NaN();
    std::string city; // Optional georeferenced GLB scene
    std::string character;
    std::string idle;
    std::string walking;
    std::string jump;
    std::string enterCar;
    std::array<std::string,32> buildingSlots{};
};

// Reads UTF-8 key=value text; relative paths are resolved from the config folder.
bool LoadGameConfig(const std::string& filename, RVGameConfig& out);
