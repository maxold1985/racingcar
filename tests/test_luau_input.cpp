#include "rv_luau.h"
#include "rv_physics.h"
#include <lua.h>
#include <lualib.h>
#include <luacode.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// Headless integration test: Luau must observe live throttle changes.
// This is the path that previously made the graphical game look frozen.
static int floorRay(void*, RVVec3 o, RVVec3 dir, float distance, RVHit* hit) {
    if (dir.y >= -1e-6f) return 0;
    float t = -o.y/dir.y;
    if (t < 0 || t > distance) return 0;
    *hit = RVHit{t, {o.x+dir.x*t, 0.0f, o.z+dir.z*t},
                    {0.0f,1.0f,0.0f}, {0.0f,0.0f,0.0f}};
    return 1;
}
static void input(lua_State* L, float throttle, float brake, float steer) {
    lua_getglobal(L, "Input");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 3);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "Input");
    }
    lua_pushnumber(L, throttle); lua_setfield(L, -2, "throttle");
    lua_pushnumber(L, brake); lua_setfield(L, -2, "brake");
    lua_pushnumber(L, steer); lua_setfield(L, -2, "steer");
    lua_pop(L, 1);
}
static void fail(lua_State* L, const char* message) {
    std::fprintf(stderr, "%s: %s\n", message, lua_tostring(L, -1));
    std::exit(1);
}
static void tick(lua_State* L) {
    lua_getglobal(L, "fixedUpdate");
    if (!lua_isfunction(L, -1)) fail(L, "Missing fixedUpdate");
    lua_pushnumber(L, 1.0/120.0);
    if (lua_pcall(L, 1, 0, 0) != 0) fail(L, "Physics update failed");
}
int main() {
    RVLuauContext context;
    context.raycast = floorRay;
    lua_State* root = luaL_newstate();
    if (!root) return 1;
    luaL_openlibs(root);
    rv_register_luau(root, &context);
    luaL_sandbox(root);
    lua_State* L = lua_newthread(root);
    luaL_sandboxthread(L);
    input(L,0,0,0);

    std::ifstream file(RV_GAME_SCRIPT_PATH, std::ios::binary);
    if (!file) { std::fprintf(stderr, "Missing game script\n"); return 1; }
    std::ostringstream out; out << file.rdbuf();
    const std::string source = out.str();
    lua_CompileOptions options{};
    const char* mutableGlobals[] = {"Input", nullptr};
    options.mutableGlobals = mutableGlobals;
    size_t bytes=0;
    char* compiled=luau_compile(source.data(),source.size(),&options,&bytes);
    if (!compiled) return 1;
    int result=luau_load(L,RV_GAME_SCRIPT_PATH,compiled,bytes,0);
    std::free(compiled);
    if (result != 0 || lua_pcall(L,0,0,0) != 0) fail(L, "Failed to load game script");

    if (context.vehicles.size() != 1) {
        std::fprintf(stderr, "Expected exactly one vehicle\n"); return 1;
    }
    RVVehicle* car = context.vehicles.begin()->second;
    for (int i=0;i<120;++i) {
        input(L,1,0,0);
        tick(L);
    }
    const RVBodyState moving=rv_vehicle_get_state(car);
    std::printf("Applied throttle: %.2f, speed: %.2f km/h, ticks: %llu\n",
                context.applied_throttle,moving.speed_kmh,context.physics_steps);
    if (std::fabs(context.applied_throttle-1.0f)>0.001f ||
        moving.speed_kmh<5.0f || context.physics_steps!=120) {
        std::fprintf(stderr, "Luau failed to apply live throttle input\n"); return 1;
    }
    for (int i=0;i<10;++i) {
        input(L,0,1,1);
        tick(L);
    }
    if (std::fabs(context.applied_throttle)>0.001f ||
        std::fabs(context.applied_brake-1.0f)>0.001f ||
        std::fabs(context.applied_steer-1.0f)>0.001f) {
        std::fprintf(stderr,"Luau retained stale keyboard values\n"); return 1;
    }
    lua_close(root);
    std::puts("PASS: Luau input mutation and vehicle movement");
    return 0;
}
