#include "rv_model.hpp"
#include "rv_luau.h"
#include "rv_physics.h"
#include "raylib.h"
#include "raymath.h"
#include <lua.h>
#include <lualib.h>
#include <luacode.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

// The graphics demo uses a flat collision plane. A future scene loader must
// replace this callback with mesh collision / BVH raycasts.
static int ground_ray(void*, RVVec3 o, RVVec3 dir, float maxDistance, RVHit* hit) {
    if (dir.y >= -1e-6f) return 0;
    const float t = -o.y / dir.y;
    if (t < 0 || t > maxDistance) return 0;
    hit->distance = t;
    hit->point = RVVec3{o.x + dir.x*t, 0, o.z + dir.z*t};
    hit->normal = RVVec3{0,1,0};
    hit->ground_velocity = RVVec3{0,0,0};
    return 1;
}
static Vector3 toRay(RVVec3 v) { return Vector3{v.x,v.y,v.z}; }
static Quaternion toRay(RVQuat q) { return Quaternion{q.x,q.y,q.z,q.w}; }

static bool loadScript(lua_State* L, const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::fprintf(stderr,"Cannot open script: %s\n",path); return false; }
    std::ostringstream out; out << in.rdbuf();
    const std::string src=out.str();
    size_t bytes=0;
    // Input is written by the C++ host while the Luau script is running.
    // Mark it mutable so GETIMPORT cannot cache stale Input.* values.
    lua_CompileOptions options{};
    const char* mutableGlobals[] = {"Input", nullptr};
    options.mutableGlobals = mutableGlobals;
    char* code=luau_compile(src.data(),src.size(),&options,&bytes);
    if (!code) return false;
    const int loadStatus=luau_load(L,path,code,bytes,0);
    std::free(code);
    if (loadStatus != 0) {
        std::fprintf(stderr,"Luau load error: %s\n",lua_tostring(L,-1));
        lua_pop(L,1);
        return false;
    }
    if (lua_pcall(L,0,0,0) != 0) {
        std::fprintf(stderr,"Luau runtime error: %s\n",lua_tostring(L,-1));
        lua_pop(L,1);
        return false;
    }
    return true;
}
static void setInput(lua_State* L, float throttle, float brake, float steer) {
    // Reuse the SAME table: Luau scripts may retain a reference to Input.
    // Replacing the global table every tick makes those references stale.
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
static bool fixedUpdate(lua_State* L, float dt) {
    lua_getglobal(L,"fixedUpdate");
    if (!lua_isfunction(L,-1)) {
        std::fprintf(stderr,"Missing fixedUpdate(dt) in script\n");
        lua_pop(L,1);
        return false;
    }
    lua_pushnumber(L,dt);
    if (lua_pcall(L,1,0,0) != 0) {
        std::fprintf(stderr,"Luau update error: %s\n",lua_tostring(L,-1));
        lua_pop(L,1);
        return false;
    }
    return true;
}
static int readVehicleId(lua_State* L) {
    lua_getglobal(L,"vehicleId");
    const int id=lua_isnumber(L,-1) ? static_cast<int>(lua_tointeger(L,-1)) : 0;
    lua_pop(L,1);
    return id;
}
#ifndef RV_DEFAULT_GAME_SCRIPT
#define RV_DEFAULT_GAME_SCRIPT "scripts/racing_game.luau"
#endif

int main(int argc, char** argv) {
    const char* chassisPath=argc>1 ? argv[1] : nullptr;
    const char* scriptPath=argc>2 ? argv[2] : RV_DEFAULT_GAME_SCRIPT;

    InitWindow(1280,720,"racingcar - Luau raycast racing");
    if (!IsWindowReady()) return 1;
    SetTargetFPS(120);

    RVModel chassis, wheel;
    if (!chassisPath || !chassis.load(chassisPath)) {
        if (chassisPath) std::fprintf(stderr,"Using fallback car body\n");
        chassis.makeCube(1.7f,0.55f,3.5f);
    }
    wheel.makeWheel(0.34f,0.20f);

    int status=0;
    {
        // Keep context alive until the VM has finished.
        RVLuauContext ctx;
        ctx.raycast=ground_ray;
        lua_State* mainState=luaL_newstate();
        if (!mainState) {
            std::fprintf(stderr,"Cannot allocate Luau VM\n");
            status=1;
        } else {
            luaL_openlibs(mainState);
            rv_register_luau(mainState,&ctx);
            luaL_sandbox(mainState);
            lua_State* L=lua_newthread(mainState); // rooted on mainState's stack
            luaL_sandboxthread(L);
            setInput(L,0,0,0);

            if (!loadScript(L,scriptPath)) {
                status=1;
            } else {
                const int vehicleId=readVehicleId(L);
                auto it=ctx.vehicles.find(vehicleId);
                if (it==ctx.vehicles.end()) {
                    std::fprintf(stderr,"Script must set vehicleId = Physics.newVehicle(...)\n");
                    status=1;
                } else {
                    RVVehicle* car=it->second;
                    constexpr float fixedDt=1.0f/120.0f;
                    float accumulator=0.0f;
                    RVBodyState state=rv_vehicle_get_state(car);
                    Camera3D camera{};
                    camera.position=Vector3{0,5,-10};
                    camera.target=toRay(state.position);
                    camera.up=Vector3{0,1,0};
                    camera.fovy=65;
                    camera.projection=CAMERA_PERSPECTIVE;

                    while (!WindowShouldClose()) {
                        float frame=std::min(GetFrameTime(),0.1f);
                        accumulator+=frame;
                        const float throttle=(IsKeyDown(KEY_W)||IsKeyDown(KEY_UP)) ? 1.0f :
                                             (IsKeyDown(KEY_S)||IsKeyDown(KEY_DOWN)) ? -0.45f : 0.0f;
                        const float brake=IsKeyDown(KEY_SPACE) ? 1.0f : 0.0f;
                        const float steer=(IsKeyDown(KEY_D)||IsKeyDown(KEY_RIGHT) ? 1.0f : 0.0f)
                                         -(IsKeyDown(KEY_A)||IsKeyDown(KEY_LEFT) ? 1.0f : 0.0f);
                        int steps=0;
                        bool running=true;
                        while (accumulator>=fixedDt && steps<12) {
                            setInput(L,throttle,brake,steer);
                            if (!fixedUpdate(L,fixedDt)) { running=false; break; }
                            accumulator-=fixedDt;
                            ++steps;
                        }
                        if (!running) { status=1; break; }
                        if (steps==12 && accumulator>=fixedDt) accumulator=0; // avoid spiral of death
                        state=rv_vehicle_get_state(car);
                        const Vector3 pos=toRay(state.position);
                        const Quaternion orientation=toRay(state.orientation);
                        const Vector3 back=Vector3RotateByQuaternion(Vector3{0,2.8f,-8.5f},orientation);
                        const Vector3 wanted=Vector3Add(pos,back);
                        camera.position=Vector3Lerp(camera.position,wanted,0.08f);
                        camera.target=Vector3Lerp(camera.target,Vector3Add(pos,Vector3{0,0.6f,0}),0.15f);

                        int groundedCount=0;
                        int drivenGrounded=0;
                        for (int i=0;i<rv_vehicle_wheel_count(car);++i) {
                            const RVWheelState ws=rv_vehicle_get_wheel(car,i);
                            if (ws.grounded) {
                                ++groundedCount;
                                if (i>=2) ++drivenGrounded; // demo: rear wheels 3 and 4
                            }
                        }
                        BeginDrawing();
                        ClearBackground(Color{125,180,220,255});
                        BeginMode3D(camera);
                        DrawPlane(Vector3{0,-0.025f,0},Vector2{1500,1500},Color{65,100,65,255});
                        DrawGrid(80,5);
                        chassis.draw(pos,orientation,1.0f,Color{235,70,55,255});
                        for (int i=0;i<rv_vehicle_wheel_count(car);++i) {
                            RVWheelState w=rv_vehicle_get_wheel(car,i);
                            // Cylinder axis Y -> wheel axle X; steering about chassis up,
                            // spin about local X after steering.
                            Quaternion turn=QuaternionFromAxisAngle(Vector3{0,1,0},w.steer_radians);
                            Quaternion axle=QuaternionFromAxisAngle(Vector3{0,0,1},PI/2);
                            Quaternion spin=QuaternionFromAxisAngle(Vector3{1,0,0},w.spin_radians);
                            Quaternion q=QuaternionMultiply(orientation,QuaternionMultiply(turn,QuaternionMultiply(axle,spin)));
                            wheel.draw(toRay(w.wheel_center),q,1.0f,DARKGRAY);
                        }
                        EndMode3D();
                        DrawRectangle(10,10,670,200,Fade(BLACK,0.70f));
                        DrawText(TextFormat("Speed: %.1f km/h",state.speed_kmh),20,20,24,RAYWHITE);
                        DrawText("W/S accelerate/reverse  A/D steer  SPACE brake",20,54,16,RAYWHITE);
                        DrawText("ESC quit  |  model: GLB, glTF, OBJ, IQM",20,75,16,RAYWHITE);
                        DrawText(TextFormat("INPUT: throttle %.2f | brake %.2f | steer %.2f",
                                            throttle,brake,steer),20,100,18,YELLOW);
                        DrawText(TextFormat("CONTACT: %d/%d wheels | rear driven grounded: %d/2",
                                            groundedCount,rv_vehicle_wheel_count(car),drivenGrounded),
                                 20,124,18,drivenGrounded>0?GREEN:ORANGE);
                        DrawText(TextFormat("LUAU: throttle %.2f | brake %.2f | steer %.2f",
                                            ctx.applied_throttle,ctx.applied_brake,ctx.applied_steer),
                                 20,148,18,WHITE);
                        DrawText(TextFormat("VM physics steps: %llu | focused: %s",
                                            ctx.physics_steps,IsWindowFocused()?"YES":"NO"),
                                 20,173,18,ctx.physics_steps>0?GREEN:RED);
                        DrawFPS(GetScreenWidth()-100,20);
                        EndDrawing();
                    }
                }
            }
            lua_close(mainState);
        }
    }
    wheel.unload();
    chassis.unload();
    CloseWindow();
    return status;
}
