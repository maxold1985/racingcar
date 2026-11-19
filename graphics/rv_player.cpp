#include "rv_player.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

bool RVPlayer::load(const char* modelPath) {
    if (modelPath && modelPath[0] && model_.load(modelPath)) {
        std::fprintf(stderr,"Character model loaded: %s\n",modelPath);
        return true;
    }
    std::fprintf(stderr,"Character: using built-in placeholder; optional 4th argument is character GLB\n");
    return false;
}
void RVPlayer::spawn(Vector3 position) {
    position_ = position;
    position_.y = 0.0f;
}
void RVPlayer::update(float dt, const Camera3D& camera) {
    if (insideCar) return;
    Vector3 forward = Vector3Subtract(camera.target,camera.position);
    forward.y=0.0f;
    if (Vector3LengthSqr(forward)<0.0001f) forward=Vector3{0,0,1};
    forward=Vector3Normalize(forward);
    const Vector3 right=Vector3{forward.z,0,-forward.x};
    const float fb=(IsKeyDown(KEY_W)||IsKeyDown(KEY_UP)?1.0f:0.0f)
                  -(IsKeyDown(KEY_S)||IsKeyDown(KEY_DOWN)?1.0f:0.0f);
    const float lr=(IsKeyDown(KEY_D)||IsKeyDown(KEY_RIGHT)?1.0f:0.0f)
                  -(IsKeyDown(KEY_A)||IsKeyDown(KEY_LEFT)?1.0f:0.0f);
    Vector3 movement=Vector3Add(Vector3Scale(forward,fb),Vector3Scale(right,lr));
    if (Vector3LengthSqr(movement)>0.0001f) {
        movement=Vector3Normalize(movement);
        position_=Vector3Add(position_,Vector3Scale(movement,4.0f*dt));
        facing_=std::atan2(movement.x,movement.z);
        walkPhase_+=dt*11.0f;
    } else walkPhase_=0.0f;
    position_.y=0.0f; // demo collision plane
}
void RVPlayer::draw() const {
    if (insideCar) return;
    const Vector3 center=Vector3Add(position_,Vector3{0,0.9f,0});
    if (model_.loaded()) {
        const Quaternion q=QuaternionFromAxisAngle(Vector3{0,1,0},facing_);
        // RVModel normalizes the longest dimension to 3.5 m; 0.5 gives ~1.75 m.
        model_.draw(center,q,0.5f,WHITE);
    } else {
        // Visible placeholder until the user supplies a character GLB.
        DrawCylinder(Vector3Add(position_,Vector3{0,0.85f,0}),0.24f,0.24f,1.25f,12,BLUE);
        DrawSphere(Vector3Add(position_,Vector3{0,1.65f,0}),0.25f,BEIGE);
        DrawCube(Vector3Add(position_,Vector3{-0.13f,0.20f,0}),0.17f,0.40f,0.2f,DARKBLUE);
        DrawCube(Vector3Add(position_,Vector3{0.13f,0.20f,0}),0.17f,0.40f,0.2f,DARKBLUE);
    }
}
void RVPlayer::unload() { model_.unload(); }
