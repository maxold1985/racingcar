#include "rv_model.hpp"
#include "rv_player.hpp"
#include "rv_game_config.hpp"
#include "webp_texture.hpp"
#include "rv_luau.h"
#include "rv_physics.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <lua.h>
#include <lualib.h>
#include <luacode.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
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
    RVGameConfig config;
    std::string configFile;
    if(argc>1 && IsFileExtension(argv[1],".txt")) {
        configFile=argv[1];
    } else if(argc==1) {
        // Resolve game.txt independently of the process working directory.
        // The source-tree path is injected by CMake for IDE/VS launches.
        const char* candidates[]={
            "game.txt","../game.txt","../../game.txt","../../../game.txt",
#ifdef RV_SOURCE_GAME_CONFIG
            RV_SOURCE_GAME_CONFIG,
#endif
        };
        for(const char* candidate:candidates) {
            if(FileExists(candidate)) { configFile=candidate;break; }
        }
    }
    if(!configFile.empty()) {
        std::fprintf(stderr,"Using game config: %s\n",configFile.c_str());
        if(!LoadGameConfig(configFile,config)) return 1;
    } else if(argc>1) {
        // Backward-compatible positional arguments.
        config.car=argv[1];
        config.script=argc>2?argv[2]:RV_DEFAULT_GAME_SCRIPT;
        config.pavement=argc>3?argv[3]:"../../assets/pavimento.webp";
        if(argc>4) config.character=argv[4];
        if(argc>5) config.idle=argv[5];
        if(argc>6) config.walking=argv[6];
        if(argc>7) config.enterCar=argv[7];
        if(argc>8) config.jump=argv[8];
    } else {
        std::fprintf(stderr,"No game.txt found; run from repository root or build/Release.\n");
        return 1;
    }
    const char* chassisPath=config.car.empty()?nullptr:config.car.c_str();
    const char* scriptPath=config.script.c_str();
    const char* pavementPath=config.pavement.c_str();
    const char* characterPath=config.character.empty()?nullptr:config.character.c_str();
    const char* idlePath=config.idle.empty()?nullptr:config.idle.c_str();
    const char* walkingPath=config.walking.empty()?nullptr:config.walking.c_str();
    const char* jumpPath=config.jump.empty()?nullptr:config.jump.c_str();
    std::fprintf(stderr,"Jump animation config: %s\n",jumpPath?jumpPath:"(none)");
    const char* enterPath=config.enterCar.empty()?nullptr:config.enterCar.c_str();

    InitWindow(1280,720,"racingcar - Luau raycast racing");
    if (!IsWindowReady()) return 1;
    SetTargetFPS(120);

    Texture2D pavement{};
    bool pavementLoaded=false;
    std::fprintf(stderr, "Working directory: %s\n", GetWorkingDirectory());
    std::fprintf(stderr, "Pavement path: %s\n", pavementPath);
    if (FileExists(pavementPath)) {
        pavement=LoadWebPTexture(pavementPath);
        pavementLoaded=IsTextureValid(pavement);
        if (pavementLoaded) {
            SetTextureFilter(pavement,TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(pavement,TEXTURE_WRAP_REPEAT);
            std::fprintf(stderr,"Pavement loaded: %s (%dx%d), GPU texture %u\n",
                         pavementPath,pavement.width,pavement.height,pavement.id);
        }
    } else {
        std::fprintf(stderr,"Pavement file does not exist: %s\n",pavementPath);
    }
    if (!pavementLoaded)
        std::fprintf(stderr,"Pavement WebP unavailable: %s; using solid ground\n",pavementPath);

    // Use raylib's mesh/material renderer rather than raw rlgl immediate mode.
    // Its shader receives the texture and UVs through the normal model pipeline.
    Model pavementModel{};
    if (pavementLoaded) {
        Mesh mesh=GenMeshPlane(1500.0f,1500.0f,1,1);
        for (int i=0; i<mesh.vertexCount; ++i) {
            mesh.texcoords[i*2] *= 300.0f;
            mesh.texcoords[i*2+1] *= 300.0f;
        }
        UpdateMeshBuffer(mesh, 1, mesh.texcoords, mesh.vertexCount*2*sizeof(float), 0);
        pavementModel=LoadModelFromMesh(mesh);
        // Replace only the default albedo map. Do not unload the same texture twice.
        pavementModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture=pavement;
    }

    RVModel chassis;
    // The imported GLB already includes its own wheels. Keep simulated tires
    // for suspension/traction, but do not draw extra cylinder meshes.
    if (!chassisPath || !chassis.load(chassisPath)) {
        if (chassisPath) std::fprintf(stderr,"Using fallback car body\n");
        chassis.makeCube(1.7f,0.55f,3.5f);
    }

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
                    RVPlayer player;
                    player.load(characterPath);
                    if (idlePath) player.loadAnimation("Idle",idlePath);
                    if (walkingPath) player.loadAnimation("Walking",walkingPath);
                    if (jumpPath) player.loadAnimation("Jump",jumpPath);
                    if (enterPath) player.loadAnimation("EnterCar",enterPath);
                    if (!player.playAnimation("Idle",true))
                        player.playAnimation("BindPose",true);
                    player.spawn(Vector3Add(toRay(state.position),Vector3{-3.0f,0.0f,0.0f}));
                    // Driver door offset in vehicle-local coordinates; configurable per car model.
                    const Vector3 doorLocal{-1.05f,0.0f,0.35f};
                    constexpr float enterRadius=1.7f;
                    Camera3D camera{};
                    camera.position=Vector3{0,5,-10};
                    camera.target=toRay(state.position);
                    camera.up=Vector3{0,1,0};
                    camera.fovy=65;
                    camera.projection=CAMERA_PERSPECTIVE;
                    // Orbit around the car with right mouse drag; wheel zooms.
                    // Yaw is world-space so chassis roll never flips the camera.
                    float orbitYaw=0.0f;
                    float orbitPitch=0.28f;
                    float orbitDistance=9.0f;
                    Vector3 cameraFocus=Vector3Add(toRay(state.position),Vector3{0,0.6f,0});

                    struct GrabBox {
                        Vector3 position;
                        Vector3 velocity{0,0,0};
                        float size=0.85f;
                    };
                    std::vector<GrabBox> grabBoxes;
                    // Spawn demo boxes on the ground near the car.
                    for(int i=0;i<12;++i) {
                        const int column=i%4, row=i/4;
                        grabBoxes.push_back(GrabBox{
                            Vector3{toRay(state.position).x+5.0f+column*1.8f,0.425f,toRay(state.position).z-2.0f+row*2.0f}
                        });
                    }
                    int grabbedBox=-1;
                    float grabDistance=5.0f;
                    Vector3 grabEnd{0,0,0};
                    bool showGrabRay=false;

                    while (!WindowShouldClose()) {
                        float frame=std::min(GetFrameTime(),0.1f);
                        accumulator+=frame;
                        if (player.insideCar && IsKeyPressed(KEY_R)) {
                            // Manual recovery from rollovers or unstable states.
                            rv_vehicle_set_pose(car,RVVec3{0,1.0f,0},RVQuat{0,0,0,1});
                            rv_vehicle_set_velocity(car,RVVec3{0,0,0},RVVec3{0,0,0});
                            accumulator=0.0f;
                        }
                        const RVBodyState interactionState=rv_vehicle_get_state(car);
                        const Quaternion carRotation=toRay(interactionState.orientation);
                        Vector3 door=Vector3Add(toRay(interactionState.position),
                            Vector3RotateByQuaternion(doorLocal,carRotation));
                        door.y=0.0f;
                        const bool nearDoor=!player.insideCar &&
                            Vector3Distance(player.position(),door)<=enterRadius;
                        // E enters at the door; Space jumps on foot and brakes while driving.
                        if (nearDoor && IsKeyPressed(KEY_E)) {
                            player.insideCar=true;
                            accumulator=0.0f;
                        } else if (player.insideCar && IsKeyPressed(KEY_F)) {
                            player.insideCar=false;
                            player.spawn(Vector3Add(door,Vector3{-0.65f,0,0}));
                            accumulator=0.0f;
                        }
                        if (!player.insideCar) player.update(frame,camera);
                        // Block walking through the car even before physics advances.
                        player.resolveCarCollision(toRay(interactionState.position),
                                                   carRotation,chassis.collisionHalfExtents());
                        const float throttle=player.insideCar ?
                            ((IsKeyDown(KEY_W)||IsKeyDown(KEY_UP)) ? 1.0f :
                             (IsKeyDown(KEY_S)||IsKeyDown(KEY_DOWN)) ? -0.45f : 0.0f) : 0.0f;
                        const float brake=player.insideCar ? (IsKeyDown(KEY_SPACE)?1.0f:0.0f) : 1.0f;
                        const float steer=player.insideCar ?
                            ((IsKeyDown(KEY_A)||IsKeyDown(KEY_LEFT) ? 1.0f : 0.0f)
                            -(IsKeyDown(KEY_D)||IsKeyDown(KEY_RIGHT) ? 1.0f : 0.0f)) : 0.0f;
                        int steps=0;
                        bool running=true;
                        while (accumulator>=fixedDt && steps<12) {
                            setInput(L,throttle,brake,steer);
                            if (!fixedUpdate(L,fixedDt)) { running=false; break; }
                            // Demo ground collider: the render plane is at Y=0,
                            // but wheel raycasts alone do not collide the chassis.
                            // Keep a conservative chassis clearance above the plane.
                            RVBodyState corrected=rv_vehicle_get_state(car);
                            constexpr float chassisClearance=0.35f;
                            if (corrected.position.y < chassisClearance) {
                                corrected.position.y=chassisClearance;
                                if (corrected.velocity.y < 0.0f) corrected.velocity.y=0.0f;
                                rv_vehicle_set_pose(car,corrected.position,corrected.orientation);
                                rv_vehicle_set_velocity(car,corrected.velocity,corrected.angular_velocity);
                            }
                            accumulator-=fixedDt;
                            ++steps;
                        }
                        if (!running) { status=1; break; }
                        if (steps==12 && accumulator>=fixedDt) accumulator=0; // avoid spiral of death
                        state=rv_vehicle_get_state(car);
                        const Vector3 pos=toRay(state.position);
                        const Quaternion orientation=toRay(state.orientation);
                        // Moving car also pushes an on-foot player out of overlap.
                        player.resolveCarCollision(pos,orientation,
                                                   chassis.collisionHalfExtents());
                        // Camera follows chassis position but not its pitch/roll.
                        // Mouse drag: orbit. Mouse wheel: distance. Home: reset view.
                        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
                            const Vector2 delta=GetMouseDelta();
                            orbitYaw -= delta.x*0.005f;
                            orbitPitch=std::clamp(orbitPitch+delta.y*0.005f,-1.25f,1.25f);
                        }
                        orbitDistance=std::clamp(orbitDistance-GetMouseWheelMove()*1.0f,3.0f,35.0f);
                        if (IsKeyPressed(KEY_HOME)) {
                            orbitYaw=0.0f;
                            orbitPitch=0.28f;
                            orbitDistance=9.0f;
                        }
                        const float heading=std::atan2(
                            2.0f*(orientation.w*orientation.y+orientation.x*orientation.z),
                            1.0f-2.0f*(orientation.y*orientation.y+orientation.z*orientation.z));
                        const float angle=heading+orbitYaw;
                        const float horizontal=orbitDistance*std::cos(orbitPitch);
                        const Vector3 focusWanted=player.insideCar ?
                            Vector3Add(pos,Vector3{0,0.6f,0}) :
                            Vector3Add(player.position(),Vector3{0,1.0f,0});
                        const float follow=1.0f-std::exp(-8.0f*frame);
                        cameraFocus=Vector3Lerp(cameraFocus,focusWanted,follow);
                        camera.target=cameraFocus;
                        camera.position=Vector3Add(cameraFocus,Vector3{
                            -std::sin(angle)*horizontal,
                            std::sin(orbitPitch)*orbitDistance,
                            -std::cos(angle)*horizontal
                        });

                        // Right mouse button: cast a red ray and grab the nearest box.
                        // The ray is cast from the camera through the mouse cursor.
                        const Ray grabRay=GetScreenToWorldRay(GetMousePosition(),camera);
                        showGrabRay=IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && !player.insideCar;
                        if(showGrabRay && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                            grabbedBox=-1;
                            float nearest=25.0f;
                            for(int i=0;i<static_cast<int>(grabBoxes.size());++i) {
                                const GrabBox& box=grabBoxes[i];
                                const float h=box.size*0.5f;
                                const BoundingBox bounds{
                                    Vector3{box.position.x-h,box.position.y-h,box.position.z-h},
                                    Vector3{box.position.x+h,box.position.y+h,box.position.z+h}
                                };
                                const RayCollision hit=GetRayCollisionBox(grabRay,bounds);
                                if(hit.hit && hit.distance<nearest) {
                                    nearest=hit.distance;
                                    grabbedBox=i;
                                }
                            }
                            if(grabbedBox>=0) grabDistance=nearest;
                        }
                        if(IsMouseButtonReleased(MOUSE_BUTTON_RIGHT) || player.insideCar)
                            grabbedBox=-1;
                        grabEnd=Vector3Add(grabRay.position,
                                           Vector3Scale(grabRay.direction,
                                               grabbedBox>=0?grabDistance:25.0f));
                        if(grabbedBox>=0) {
                            // Smooth critically damped follow, clamped to the ground.
                            GrabBox& held=grabBoxes[grabbedBox];
                            Vector3 target=grabEnd;
                            target.y=std::max(target.y,held.size*0.5f);
                            const float stiffness=55.0f;
                            const float damping=15.0f;
                            Vector3 accel=Vector3Subtract(
                                Vector3Scale(Vector3Subtract(target,held.position),stiffness),
                                Vector3Scale(held.velocity,damping));
                            held.velocity=Vector3Add(held.velocity,Vector3Scale(accel,frame));
                            held.position=Vector3Add(held.position,
                                                     Vector3Scale(held.velocity,frame));
                        }
                        for(int i=0;i<static_cast<int>(grabBoxes.size());++i) {
                            if(i==grabbedBox) continue;
                            GrabBox& box=grabBoxes[i];
                            box.velocity.y-=18.0f*frame;
                            box.position=Vector3Add(box.position,
                                                     Vector3Scale(box.velocity,frame));
                            const float floorY=box.size*0.5f;
                            if(box.position.y<floorY) {
                                box.position.y=floorY;
                                if(box.velocity.y<0) box.velocity.y=0;
                            }
                            box.velocity.x*=std::exp(-4.0f*frame);
                            box.velocity.z*=std::exp(-4.0f*frame);
                        }

                        // Resolve box AABB contacts after gravity/grabbing.
                        // Ground contact, box-box separation and horizontal contacts
                        // with the player and the vehicle (simple kinematic colliders).
                        for(int iteration=0;iteration<4;++iteration) {
                            for(int i=0;i<static_cast<int>(grabBoxes.size());++i) {
                                GrabBox& a=grabBoxes[i];
                                const float ha=a.size*0.5f;
                                if(a.position.y<ha) {
                                    a.position.y=ha;
                                    if(a.velocity.y<0) a.velocity.y=0;
                                }
                                for(int j=i+1;j<static_cast<int>(grabBoxes.size());++j) {
                                    GrabBox& b=grabBoxes[j];
                                    const float hb=b.size*0.5f;
                                    const float px=ha+hb-std::abs(a.position.x-b.position.x);
                                    const float py=ha+hb-std::abs(a.position.y-b.position.y);
                                    const float pz=ha+hb-std::abs(a.position.z-b.position.z);
                                    if(px<=0 || py<=0 || pz<=0) continue;
                                    // Move along the least-penetrating axis.
                                    int axis=0;
                                    float depth=px;
                                    if(py<depth) {axis=1;depth=py;}
                                    if(pz<depth) {axis=2;depth=pz;}
                                    const bool holdA=i==grabbedBox,holdB=j==grabbedBox;
                                    const float moveA=holdA?0.0f:(holdB?1.0f:0.5f);
                                    const float moveB=holdB?0.0f:(holdA?1.0f:0.5f);
                                    float* ap=axis==0?&a.position.x:(axis==1?&a.position.y:&a.position.z);
                                    float* bp=axis==0?&b.position.x:(axis==1?&b.position.y:&b.position.z);
                                    float* av=axis==0?&a.velocity.x:(axis==1?&a.velocity.y:&a.velocity.z);
                                    float* bv=axis==0?&b.velocity.x:(axis==1?&b.velocity.y:&b.velocity.z);
                                    const float sign=*ap>=*bp?1.0f:-1.0f;
                                    *ap+=sign*depth*moveA;
                                    *bp-=sign*depth*moveB;
                                    if(*av*sign<0) *av=0;
                                    if(*bv*sign>0) *bv=0;
                                }
                                if(i==grabbedBox) continue;
                                // Player: vertical capsule approximated by a circle
                                // against the crate's horizontal AABB.
                                if(!player.insideCar) {
                                    const Vector3 pp=player.position();
                                    if(a.position.y+ha>pp.y &&
                                       a.position.y-ha<pp.y+1.75f) {
                                        const float cx=std::clamp(pp.x,a.position.x-ha,a.position.x+ha);
                                        const float cz=std::clamp(pp.z,a.position.z-ha,a.position.z+ha);
                                        float dx=cx-pp.x,dz=cz-pp.z;
                                        const float distanceSq=dx*dx+dz*dz;
                                        constexpr float radius=0.30f;
                                        if(distanceSq<radius*radius) {
                                            if(distanceSq>1e-8f) {
                                                const float d=std::sqrt(distanceSq);
                                                a.position.x+=dx/d*(radius-d);
                                                a.position.z+=dz/d*(radius-d);
                                            } else {
                                                const float xSide=ha+radius-std::abs(a.position.x-pp.x);
                                                const float zSide=ha+radius-std::abs(a.position.z-pp.z);
                                                if(xSide<zSide) a.position.x+=(a.position.x>=pp.x?xSide:-xSide);
                                                else a.position.z+=(a.position.z>=pp.z?zSide:-zSide);
                                            }
                                            a.velocity.x=0;
                                            a.velocity.z=0;
                                        }
                                    }
                                }
                                // Vehicle: conservative world AABB of its oriented body.
                                const Vector3 ch=chassis.collisionHalfExtents();
                                const Vector3 cpos=Vector3Add(pos,Vector3{0,-0.30f,0});
                                const Vector3 ex=Vector3RotateByQuaternion(Vector3{ch.x,0,0},orientation);
                                const Vector3 ey=Vector3RotateByQuaternion(Vector3{0,ch.y,0},orientation);
                                const Vector3 ez=Vector3RotateByQuaternion(Vector3{0,0,ch.z},orientation);
                                const Vector3 extent{
                                    std::abs(ex.x)+std::abs(ey.x)+std::abs(ez.x),
                                    std::abs(ex.y)+std::abs(ey.y)+std::abs(ez.y),
                                    std::abs(ex.z)+std::abs(ey.z)+std::abs(ez.z)
                                };
                                const float px=ha+extent.x-std::abs(a.position.x-cpos.x);
                                const float py=ha+extent.y-std::abs(a.position.y-cpos.y);
                                const float pz=ha+extent.z-std::abs(a.position.z-cpos.z);
                                if(px>0 && py>0 && pz>0) {
                                    if(px<=py && px<=pz) {
                                        a.position.x+=(a.position.x>=cpos.x?px:-px);
                                        a.velocity.x=0;
                                    } else if(py<=pz) {
                                        a.position.y+=(a.position.y>=cpos.y?py:-py);
                                        a.velocity.y=0;
                                    } else {
                                        a.position.z+=(a.position.z>=cpos.z?pz:-pz);
                                        a.velocity.z=0;
                                    }
                                }
                            }
                        }

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
                        if (pavementLoaded) {
                            DrawModel(pavementModel,Vector3{0,-0.025f,0},1.0f,WHITE);
                        } else {
                            DrawPlane(Vector3{0,-0.025f,0},Vector2{1500,1500},Color{65,100,65,255});
                            DrawGrid(80,5);
                        }
                        // WHITE preserves the imported glTF/GLB base-color textures and materials.
                        // A red tint multiplies every material color and makes the car red.
                        // The visible wheels are part of the GLB chassis mesh;
                        // physics wheels are intentionally not rendered. Lower
                        // only the visual model to bring its tires closer to the
                        // ground without changing suspension, raycasts or COM.
                        constexpr float carVisualDrop=0.30f;
                        const Vector3 carVisualPosition=Vector3Add(
                            pos,Vector3{0.0f,-carVisualDrop,0.0f});
                        chassis.draw(carVisualPosition,orientation,1.0f,WHITE);
                        player.draw();
                        // Green fake contact shadow: footprint stays on the ground
                        // even when a crate is lifted by the red ray.
                        for(const GrabBox& box:grabBoxes) {
                            const float footprint=box.size*0.94f;
                            const float elevation=std::max(0.0f,box.position.y-box.size*0.5f);
                            const float alpha=std::clamp(0.55f/(1.0f+elevation*0.35f),0.12f,0.55f);
                            const Vector3 ground{box.position.x,0.012f,box.position.z};
                            DrawCube(ground,footprint,0.008f,footprint,
                                     Fade(Color{25,225,65,255},alpha));
                            DrawCubeWires(ground,footprint,0.008f,footprint,
                                          Fade(DARKGREEN,0.8f));
                        }
                        for(int i=0;i<static_cast<int>(grabBoxes.size());++i) {
                            const GrabBox& box=grabBoxes[i];
                            const Color color=i==grabbedBox?ORANGE:Color{180,135,85,255};
                            DrawCube(box.position,box.size,box.size,box.size,color);
                            DrawCubeWires(box.position,box.size,box.size,box.size,DARKBROWN);
                        }
                        if(showGrabRay) {
                            DrawLine3D(grabRay.position,grabEnd,RED);
                            DrawSphere(grabEnd,0.07f,RED);
                        }
                        if (!player.insideCar) {
                            const Vector3 marker=Vector3Add(door,Vector3{0,0.12f,0});
                            DrawCylinder(marker,0.62f,0.62f,0.06f,24,Fade(SKYBLUE,0.45f));
                            DrawCylinderWires(marker,0.62f,0.62f,0.06f,24,BLUE);
                            DrawSphere(Vector3Add(door,Vector3{0,1.5f,0}),0.14f,
                                       nearDoor?GREEN:SKYBLUE);
                        }
                        // Tire meshes intentionally hidden. Physics raycasts,
                        // steering, anti-roll and tire forces still run normally.
                        EndMode3D();
                        DrawRectangle(10,10,670,226,Fade(BLACK,0.70f));
                        DrawText(TextFormat("Speed: %.1f km/h",state.speed_kmh),20,20,24,RAYWHITE);
                        DrawText(player.insideCar ?
                            "DRIVING: W/S accelerate | A/D steer | SPACE brake | F exit" :
                            "ON FOOT: WASD move | SPACE jump | E enter | RMB grab boxes",20,54,16,RAYWHITE);
                        DrawText("R reset | MMB orbit | RMB red ray/grab | Wheel zoom | HOME camera | ESC quit",20,75,16,RAYWHITE);
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
                        DrawText(pavementLoaded ? "PAVEMENT: WEBP OK" : "PAVEMENT: NOT LOADED",20,198,18,pavementLoaded?GREEN:RED);
                        if (nearDoor) DrawText("E: ENTER CAR",GetScreenWidth()/2-110,GetScreenHeight()-85,24,GREEN);
                        DrawFPS(GetScreenWidth()-100,20);
                        EndDrawing();
                    }
                    player.unload();
                }
            }
            lua_close(mainState);
        }
    }
    chassis.unload();
    if (pavementLoaded) {
        // UnloadModel owns its material textures, including pavement.
        UnloadModel(pavementModel);
    }
    CloseWindow();
    return status;
}
