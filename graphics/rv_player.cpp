#include "rv_player.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

bool RVPlayer::load(const char* modelPath) {
    if (modelPath && modelPath[0] && animation_.loadCharacter(modelPath)) {
        animation_.play("BindPose",true);
        return true;
    }
    if (modelPath && modelPath[0] && model_.load(modelPath)) {
        std::fprintf(stderr,"Character model loaded: %s\n",modelPath);
        return true;
    }
    std::fprintf(stderr,"Character: using built-in placeholder; optional 4th argument is character GLB\n");
    return false;
}
bool RVPlayer::loadAnimation(const std::string& name,const char* path) {
    return animation_.loadClip(name,path);
}
bool RVPlayer::playAnimation(const std::string& name,bool loop) {
    return animation_.play(name,loop);
}
void RVPlayer::spawn(Vector3 position) {
    position_ = position;
    position_.y = 0.0f;
    verticalVelocity_=0.0f;
    grounded_=true;
}
void RVPlayer::update(float dt, const Camera3D& camera) {
    if (insideCar) return;
    // Semi-implicit Euler jump with a single ground plane at Y=0.
    constexpr float jumpSpeed=6.5f;
    constexpr float gravity=18.0f;
    if(grounded_ && IsKeyPressed(KEY_SPACE)) {
        verticalVelocity_=jumpSpeed;
        grounded_=false;
    }
    if(!grounded_) {
        verticalVelocity_-=gravity*dt;
        position_.y+=verticalVelocity_*dt;
        if(position_.y<=0.0f) {
            position_.y=0.0f;
            verticalVelocity_=0.0f;
            grounded_=true;
        }
    }
    Vector3 forward = Vector3Subtract(camera.target,camera.position);
    forward.y=0.0f;
    if (Vector3LengthSqr(forward)<0.0001f) forward=Vector3{0,0,1};
    forward=Vector3Normalize(forward);
    const Vector3 right=Vector3{forward.z,0,-forward.x};
    const float fb=(IsKeyDown(KEY_W)||IsKeyDown(KEY_UP)?1.0f:0.0f)
                  -(IsKeyDown(KEY_S)||IsKeyDown(KEY_DOWN)?1.0f:0.0f);
    const float lr=(IsKeyDown(KEY_A)||IsKeyDown(KEY_LEFT)?1.0f:0.0f)
                  -(IsKeyDown(KEY_D)||IsKeyDown(KEY_RIGHT)?1.0f:0.0f);
    Vector3 movement=Vector3Add(Vector3Scale(forward,fb),Vector3Scale(right,lr));
    if (Vector3LengthSqr(movement)>0.0001f) {
        movement=Vector3Normalize(movement);
        position_=Vector3Add(position_,Vector3Scale(movement,4.0f*dt));
        facing_=std::atan2(movement.x,movement.z);
        walkPhase_+=dt*11.0f;
        if (!walking_) {
            if (!animation_.play("Walking",true)) animation_.play("Default",true);
            walking_=true;
        }
    } else {
        walkPhase_=0.0f;
        if (walking_) {
            if (!animation_.play("Idle",true)) animation_.play("BindPose",true);
            walking_=false;
        }
    }
    animation_.update(dt);
    // Vertical position is resolved by jump integration above.
}
// Player capsule approximated by a horizontal circle against the car OBB.
// This is a kinematic contact: the vehicle physics is left untouched.
void RVPlayer::resolveCarCollision(Vector3 carPosition, Quaternion carRotation,
                                   Vector3 carHalfExtents) {
    if(insideCar) return;
    constexpr float playerRadius=0.30f;
    constexpr float playerHeight=1.75f;
    constexpr float visualDrop=0.30f;
    // The GLB is drawn lower than the physical chassis.
    carPosition.y-=visualDrop;
    if(carPosition.y+carHalfExtents.y<position_.y ||
       carPosition.y-carHalfExtents.y>position_.y+playerHeight) return;
    const Quaternion inverse=QuaternionInvert(QuaternionNormalize(carRotation));
    Vector3 local=Vector3RotateByQuaternion(
        Vector3Subtract(position_,carPosition),inverse);
    const float cx=std::clamp(local.x,-carHalfExtents.x,carHalfExtents.x);
    const float cz=std::clamp(local.z,-carHalfExtents.z,carHalfExtents.z);
    const float dx=local.x-cx,dz=local.z-cz;
    const float distSq=dx*dx+dz*dz;
    if(distSq>=playerRadius*playerRadius) return;
    if(distSq>1e-8f) {
        const float distance=std::sqrt(distSq);
        local.x+=(dx/distance)*(playerRadius-distance);
        local.z+=(dz/distance)*(playerRadius-distance);
    } else {
        // Inside the OBB: push through the nearest side, not the vehicle.
        const float xPen=carHalfExtents.x+playerRadius-std::abs(local.x);
        const float zPen=carHalfExtents.z+playerRadius-std::abs(local.z);
        if(xPen<zPen) local.x=(local.x>=0?1.0f:-1.0f)*
                                 (carHalfExtents.x+playerRadius);
        else local.z=(local.z>=0?1.0f:-1.0f)*
                           (carHalfExtents.z+playerRadius);
    }
    const Vector3 corrected=Vector3Add(carPosition,
        Vector3RotateByQuaternion(local,carRotation));
    position_.x=corrected.x;
    position_.z=corrected.z;
}
void RVPlayer::draw() const {
    if (insideCar) return;
    const Vector3 center=Vector3Add(position_,Vector3{0,0.9f,0});
    if (animation_.loaded()) {
        animation_.draw(position_,facing_);
    } else if (model_.loaded()) {
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
void RVPlayer::unload() { animation_.unload(); model_.unload(); }
