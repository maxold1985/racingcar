#pragma once
#include "rv_model.hpp"
#include "raylib.h"
#include "raymath.h"

// Character controller for the flat demo terrain. Not a rigid-body capsule.
class RVPlayer {
public:
    bool load(const char* modelPath);
    void spawn(Vector3 position);
    void update(float dt, const Camera3D& camera);
    void draw() const;
    void unload();
    Vector3 position() const { return position_; }
    bool insideCar = false;
private:
    RVModel model_;
    Vector3 position_{0.0f,0.0f,0.0f};
    float facing_ = 0.0f;
    float walkPhase_ = 0.0f;
};
