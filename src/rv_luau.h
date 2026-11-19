#pragma once
#include "rv_physics.h"
#include <unordered_map>
struct lua_State;

/* Each Luau VM gets its own registry; a host game supplies the raycast query.
   Keep this struct alive as long as the Luau state can call Physics.* */
struct RVLuauContext {
    RVRaycastFn raycast = nullptr;
    void* raycast_user = nullptr;
    std::unordered_map<int, RVVehicle*> vehicles;
    int next_id = 1;
    // Diagnostics: values actually applied by Physics.setControls() in Luau.
    float applied_throttle = 0.0f;
    float applied_brake = 0.0f;
    float applied_steer = 0.0f;
    unsigned long long control_calls = 0;
    unsigned long long physics_steps = 0;
    ~RVLuauContext();
};

void rv_register_luau(lua_State* L, RVLuauContext* context);
