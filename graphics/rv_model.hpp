#pragma once
#include "raylib.h"
#include <string>

// RAII model resource: loaded from glTF/GLB/OBJ/IQM (as supported by raylib).
// Fallback geometry keeps the demo runnable without external assets.
class RVModel {
public:
    RVModel() = default;
    ~RVModel() { unload(); }
    RVModel(const RVModel&) = delete;
    RVModel& operator=(const RVModel&) = delete;

    bool load(const std::string& path);
    void makeCube(float width, float height, float length);
    void makeWheel(float radius, float width);
    void unload();
    void draw(Vector3 position, Quaternion rotation, float scale, Color tint) const;
    bool loaded() const { return active; }
private:
    Model model{};
    bool active = false;
};
