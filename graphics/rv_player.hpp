#pragma once
#include "rv_model.hpp"
#include "rv_animation.hpp"
#include "raylib.h"
#include "raymath.h"

// Character controller for the flat demo terrain. Not a rigid-body capsule.
class RVPlayer {
public:
    bool load(const char* modelPath);
    bool loadAnimation(const std::string& name, const char* path);
    bool playAnimation(const std::string& name, bool loop);
    void spawn(Vector3 position);
    void update(float dt, const Camera3D& camera);
    void draw() const;
    void resolveCarCollision(Vector3 carPosition, Quaternion carRotation,
                             Vector3 carHalfExtents);
    void unload();
    Vector3 position() const { return position_; }
    bool insideCar = false;
private:
    RVModel model_;
    RVAnimation animation_;
    bool walking_ = false;
    bool grounded_ = true;
    float verticalVelocity_ = 0.0f;
    Vector3 position_{0.0f,0.0f,0.0f};
    float facing_ = 0.0f;
    float walkPhase_ = 0.0f;
};
