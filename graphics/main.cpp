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
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <sstream>
#include <string>

// The graphics demo uses a flat collision plane. A future scene loader must
// replace this callback with mesh collision / BVH raycasts.

#include <unordered_map>
#include <cstdint>
#include <memory>

// City streaming: chunks are Luau-owned, sectors are native draw buckets.
struct CityInstance { int model; Vector3 position; float rotation; float scale; float scaleX; float scaleZ; };
struct CitySector { std::vector<CityInstance> instances; };
struct CityChunk { std::unordered_map<std::int64_t,CitySector> sectors; std::size_t count=0; };
static std::unordered_map<int,CityChunk> cityChunks;
static Shader cityFogShader{};
static int cityFogCameraLoc=-1;
static const char* cityFogVertex=R"GLSL(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 worldPosition;
void main() {
    worldPosition=(matModel*vec4(vertexPosition,1.0)).xyz;
    fragTexCoord=vertexTexCoord;
    fragColor=vertexColor;
    gl_Position=mvp*vec4(vertexPosition,1.0);
}
)GLSL";
static const char* cityFogFragment=R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 worldPosition;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 fogCamera;
out vec4 finalColor;
void main() {
    vec4 surface=texture(texture0,fragTexCoord)*colDiffuse*fragColor;
    float distanceToCamera=distance(worldPosition.xz,fogCamera.xz);
    float fog=smoothstep(70.0,195.0,distanceToCamera);
    finalColor=vec4(mix(surface.rgb,vec3(0.49,0.706,0.863),fog),surface.a);
}
)GLSL";
static std::unordered_map<int,Model> cityModels;
static std::unordered_map<int,BoundingBox> cityBounds;
static std::unordered_map<int,bool> cityMissing;
static const char* cityNames[] = {
    "Street_2Lane", "Street_4WayIntersection", "Street_TIntersection",
    "Building_Small_1", "Building_Medium_2_001", "Building_Large_2",
    "Sidewalk_Straight_3m", "Sidewalk_Corner_Round_3m"
};
static std::string cityAssetDirectory = "assets/city";
static constexpr float citySectorSize=48.0f;
static constexpr float cityDrawRadius=200.0f;
static constexpr std::size_t cityInstanceLimit=20000;
static std::size_t cityInstanceCount=0;
static int citySectorCoord(float value) {
    return static_cast<int>(std::floor(value/citySectorSize));
}
static std::string resolveCityFile(const std::string& relative) {
    const std::string prefixes[] = {"", "../", "../../", "../../../"};
    for (const auto& prefix:prefixes) {
        const std::string candidate=prefix+relative;
        if (FileExists(candidate.c_str())) return candidate;
    }
    return relative;
}
static int cityAdd(lua_State* L) {
    const int chunk=static_cast<int>(luaL_checkinteger(L,1));
    const int model=static_cast<int>(luaL_checkinteger(L,2));
    if (model<1 || model>9) { luaL_error(L,"invalid city model ID"); return 0; }
    if (cityInstanceCount>=cityInstanceLimit) { luaL_error(L,"city instance limit reached"); return 0; }
    const float x=static_cast<float>(luaL_checknumber(L,3));
    const float y=static_cast<float>(luaL_checknumber(L,4));
    const float z=static_cast<float>(luaL_checknumber(L,5));
    const float rotation=static_cast<float>(luaL_optnumber(L,6,0));
    const float scale=static_cast<float>(luaL_optnumber(L,7,1));
    const float scaleX=static_cast<float>(luaL_optnumber(L,8,scale));
    const float scaleZ=static_cast<float>(luaL_optnumber(L,9,scale));
    if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(rotation)||!std::isfinite(scale)||scale<=0.0f||!std::isfinite(scaleX)||!std::isfinite(scaleZ)||scaleX<=0.0f||scaleZ<=0.0f) {
        luaL_error(L,"non-finite city transform"); return 0;
    }
    // Sectors use global coordinates, avoiding collisions at negative positions.
    const int sx=citySectorCoord(x),sz=citySectorCoord(z);
    // Use a 64-bit sector key so large/negative coordinates do not alias.
    const std::int64_t key=(static_cast<std::int64_t>(static_cast<std::uint32_t>(sx))<<32) |
        static_cast<std::uint32_t>(sz);
    // Keep the sector hash in a global map keyed by chunk and sector.
    // The sector container uses 64-bit keys (see CityChunk below).
    cityChunks[chunk].sectors[key].instances.push_back({model,{x,y,z},rotation,scale,scaleX,scaleZ});
    ++cityChunks[chunk].count;
    ++cityInstanceCount;
    // Log only the first few instances per road type to avoid flooding stdout.
    if (model==1 || model==2) {
        static int loggedStraight=0;
        static int loggedIntersection=0;
        int& logged=(model==1)?loggedStraight:loggedIntersection;
        if (logged<12) {
            std::fprintf(stderr,
                "[CITY ROTATION] model=%s chunk=%d pos=(%.2f, %.2f, %.2f) yaw=%.1f deg scale=%.3f\\n",
                model==1?"Street_2Lane":"Street_4WayIntersection",
                chunk, x, y, z, rotation, scale);
            ++logged;
        }
    }
    return 0;
}
static int cityRemove(lua_State* L) {
    const int chunk=static_cast<int>(luaL_checkinteger(L,1));
    auto it=cityChunks.find(chunk);
    if (it!=cityChunks.end()) {
        cityInstanceCount-=it->second.count;
        cityChunks.erase(it);
    }
    return 0;
}
static void registerCity(lua_State* L) {
    lua_createtable(L,0,2);
    lua_pushcfunction(L,cityAdd,"add");lua_setfield(L,-2,"add");
    lua_pushcfunction(L,cityRemove,"remove");lua_setfield(L,-2,"remove");
    lua_setglobal(L,"CityWorld");
}
static void loadOneCityModel() {
    // Amortize GLB uploads: at most one new model per frame.
    for (const auto& chunk:cityChunks) {
        for (const auto& sector:chunk.second.sectors) {
            for (const CityInstance& instance:sector.second.instances) {
                if (instance.model==9 || cityModels.count(instance.model)||cityMissing.count(instance.model)) continue;
                const std::string file=resolveCityFile(cityAssetDirectory+"/"+cityNames[instance.model-1]+".glb");
                if (!FileExists(file.c_str())) {
                    cityMissing[instance.model]=true;
                    std::fprintf(stderr,"[CITY] Missing GLB: %s\\n",file.c_str());
                    return;
                }
                Model model=LoadModel(file.c_str());
                if (IsShaderValid(cityFogShader)) {
                    for(int materialIndex=0;materialIndex<model.materialCount;++materialIndex)
                        model.materials[materialIndex].shader=cityFogShader;
                }
                if (IsModelValid(model)) {
                    const BoundingBox loadedBounds=GetModelBoundingBox(model);
                    cityBounds.emplace(instance.model,loadedBounds);
                    if (instance.model==1 || instance.model==2) {
                        std::fprintf(stderr,
                            "[CITY GEOMETRY] model=%s boundsMin=(%.4f,%.4f,%.4f) boundsMax=(%.4f,%.4f,%.4f) meshes=%d\\n",
                            instance.model==1?"Street_2Lane":"Street_4WayIntersection",
                            loadedBounds.min.x,loadedBounds.min.y,loadedBounds.min.z,
                            loadedBounds.max.x,loadedBounds.max.y,loadedBounds.max.z,
                            model.meshCount);
                        std::fprintf(stderr,
                            "[CITY MATRIX] model=%d transform=[%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]\\n",
                            instance.model,
                            model.transform.m0,model.transform.m4,model.transform.m8,model.transform.m12,
                            model.transform.m1,model.transform.m5,model.transform.m9,model.transform.m13,
                            model.transform.m2,model.transform.m6,model.transform.m10,model.transform.m14,
                            model.transform.m3,model.transform.m7,model.transform.m11,model.transform.m15);
                    }
                    for (int materialIndex=0;materialIndex<model.materialCount;++materialIndex) {
                        Material& mat=model.materials[materialIndex];
                        const Texture2D diffuse=mat.maps[MATERIAL_MAP_DIFFUSE].texture;
                        if (diffuse.id!=0 && diffuse.width>0 && diffuse.height>0) {
                            GenTextureMipmaps(&mat.maps[MATERIAL_MAP_DIFFUSE].texture);
                            SetTextureFilter(mat.maps[MATERIAL_MAP_DIFFUSE].texture,TEXTURE_FILTER_TRILINEAR);
                        }
                    }
                    cityModels.emplace(instance.model,model);
                    std::fprintf(stderr,"[CITY] Loaded: %s\\n",file.c_str());
                } else {
                    cityMissing[instance.model]=true;
                    std::fprintf(stderr,"[CITY] Invalid GLB: %s\\n",file.c_str());
                }
                return;
            }
        }
    }
}
static void drawCity(const Camera3D& camera) {
    const Vector3 cameraPosition=camera.position;
    const Vector3 view={camera.target.x-camera.position.x,0.0f,camera.target.z-camera.position.z};
    const float viewLength=std::sqrt(view.x*view.x+view.z*view.z);
    const float forwardX=viewLength>0.001f?view.x/viewLength:0.0f;
    const float forwardZ=viewLength>0.001f?view.z/viewLength:0.0f;
    constexpr float buildingDetailDistance2=85.0f*85.0f;
    loadOneCityModel();
    if (IsShaderValid(cityFogShader) && cityFogCameraLoc>=0) {
        const float position[3]={camera.position.x,camera.position.y,camera.position.z};
        SetShaderValue(cityFogShader,cityFogCameraLoc,position,SHADER_UNIFORM_VEC3);
    }
    const float radius2=cityDrawRadius*cityDrawRadius;
    for (const auto& chunk:cityChunks) {
        for (const auto& sector:chunk.second.sectors) {
            const auto& instances=sector.second.instances;
            if (instances.empty()) continue;
            // Bounding sector radius includes its diagonal and an allowance for models.
            const Vector3 p=instances.front().position;
            const float sx=std::floor(p.x/citySectorSize)*citySectorSize+citySectorSize*0.5f;
            const float sz=std::floor(p.z/citySectorSize)*citySectorSize+citySectorSize*0.5f;
            const float dx=sx-cameraPosition.x,dz=sz-cameraPosition.z;
            const float sectorRadius=cityDrawRadius+citySectorSize;
            if (dx*dx+dz*dz>sectorRadius*sectorRadius) continue;
            for (const CityInstance& instance:instances) {
                const float ix=instance.position.x-cameraPosition.x;
                const float iz=instance.position.z-cameraPosition.z;
                const float distance2=ix*ix+iz*iz;
                if (distance2>radius2) continue;
                // Cull buildings sufficiently far behind the camera.
                if (instance.model>=4 && instance.model<=6 && viewLength>0.001f &&
                    ix*forwardX+iz*forwardZ < -32.0f) continue;
                if (instance.model==9) {
                    // Block occupies the 84m gap between 6m roads.
                    // Four 18x18 intersection footprints overlap its corners:
                    // cut a 6x6 square from every corner instead of covering
                    // intersection asphalt/markings with procedural pavement.
                    const float w=instance.scaleX, d=instance.scaleZ;
                    const float x=instance.position.x, z=instance.position.z;
                    const float cut=6.0f;
                    const float halfW=w*0.5f, halfD=d*0.5f;
                    const Color pavement{111,112,108,255};
                    const Color sidewalk{169,167,155,255};
                    const Color curb{135,133,123,255};
                    // Imported GLBs have minY=-0.15 and are shifted to minY=0.
                    // Their road surface is at y=0; keep block ground below it.
                    auto ground=[&](float px,float pz,float sx,float sz) {
                        if(sx>0.0f && sz>0.0f)
                            DrawCubeV({px,-0.045f,pz},{sx,0.07f,sz},pavement);
                    };
                    // Cross-shaped footprint: center strip plus two side strips.
                    ground(x,z,w-2*cut,d);
                    ground(x-halfW+cut*0.5f,z,cut,d-2*cut);
                    ground(x+halfW-cut*0.5f,z,cut,d-2*cut);
                    const float edge=1.7f;
                    auto strip=[&](float px,float pz,float sx,float sz) {
                        if(sx>0.0f && sz>0.0f)
                            DrawCubeV({px,0.005f,pz},{sx,0.09f,sz},sidewalk);
                    };
                    auto kerb=[&](float px,float pz,float sx,float sz) {
                        if(sx>0.0f && sz>0.0f)
                            DrawCubeV({px,0.005f,pz},{sx,0.10f,sz},curb);
                    };
                    // Straight outer boundaries stop 6m before each junction.
                    for(int side : {-1,1}) {
                        const float sx=x+side*(halfW-edge*0.5f);
                        const float kx=x+side*halfW;
                        strip(sx,z,edge,d-2*cut);
                        kerb(kx,z,0.2f,d-2*cut);
                        const float sz=z+side*(halfD-edge*0.5f);
                        const float kz=z+side*halfD;
                        strip(x,sz,w-2*cut,edge);
                        kerb(x,kz,w-2*cut,0.2f);
                        // Return edges around each cut-out corner.
                        for(int other : {-1,1}) {
                            const float cornerZ=z+other*(halfD-cut);
                            const float cornerX=x+other*(halfW-cut);
                            strip(x+side*(halfW-cut+edge*0.5f),cornerZ,
                                  edge,cut);
                            strip(cornerX,z+side*(halfD-cut+edge*0.5f),
                                  cut,edge);
                        }
                    }
                    continue;
                }
                const auto it=cityModels.find(instance.model);
                if (it!=cityModels.end())
                    {
                    const BoundingBox& bounds=cityBounds.at(instance.model);
                    const float cx=(bounds.min.x+bounds.max.x)*0.5f*instance.scaleX;
                    const float cz=(bounds.min.z+bounds.max.z)*0.5f*instance.scaleZ;
                    const float a=instance.rotation*DEG2RAD;
                    const float cs=std::cos(a),sn=std::sin(a);
                    const Vector3 origin={
                        instance.position.x-cx*cs-cz*sn,
                        instance.position.y-bounds.min.y*instance.scale,
                        instance.position.z+cx*sn-cz*cs
                    };
                    // Cheap distant silhouette in place of multi-material building meshes.
                    if (instance.model>=4 && instance.model<=6 && distance2>buildingDetailDistance2) {
                        const float width=(bounds.max.x-bounds.min.x)*instance.scale;
                        const float depth=(bounds.max.z-bounds.min.z)*instance.scale;
                        const float height=(bounds.max.y-bounds.min.y)*instance.scale;
                        if (width>0.0f && depth>0.0f && height>0.0f) {
                            DrawCubeV({instance.position.x,instance.position.y+height*0.5f,instance.position.z},
                                      {width,height,depth},Color{136,139,143,255});
                        }
                        continue;
                    }
                    DrawModelEx(it->second,origin,{0,1,0},instance.rotation,
                        {instance.scaleX,instance.scale,instance.scaleZ},WHITE);
                }
            }
        }
    }
}
static void unloadCity() {
    for (auto& item:cityModels) UnloadModel(item.second);
    cityModels.clear();cityBounds.clear();cityMissing.clear();cityChunks.clear();cityInstanceCount=0;
}
static void updateCityScript(lua_State* L,Vector3 p) {
    lua_getglobal(L,"cityUpdate");
    if (!lua_isfunction(L,-1)) {lua_pop(L,1);return;}
    lua_pushnumber(L,p.x);lua_pushnumber(L,p.z);
    if (lua_pcall(L,2,0,0)!=0) {
        std::fprintf(stderr,"[CITY] Luau error: %s\\n",lua_tostring(L,-1));
        lua_pop(L,1);
    }
}

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
    cityFogShader=LoadShaderFromMemory(cityFogVertex,cityFogFragment);
    if (IsShaderValid(cityFogShader))
        cityFogCameraLoc=GetShaderLocation(cityFogShader,"fogCamera");

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
            registerCity(mainState);
            luaL_sandbox(mainState);
            lua_State* L=lua_newthread(mainState); // rooted on mainState's stack
            luaL_sandboxthread(L);
            setInput(L,0,0,0);

            if (!loadScript(L,scriptPath)) {
                status=1;
            } else {
                // Optional procedural city: loaded after the vehicle script.
                const std::string metricsScript=resolveCityFile("scripts/city_asset_metrics.luau");
                if (FileExists(metricsScript.c_str())) loadScript(L,metricsScript.c_str());
                else std::fprintf(stderr,"[CITY] Asset metrics missing; run Blender converter\\n");
                const std::string cityScript=resolveCityFile("scripts/city_generator.luau");
                if (FileExists(cityScript.c_str())) {
                    if (loadScript(L,cityScript.c_str()))
                        std::fprintf(stderr,"[CITY] Luau generator loaded: %s\\n",cityScript.c_str());
                } else {
                    std::fprintf(stderr,"[CITY] Generator missing: %s\\n",cityScript.c_str());
                }
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

                    // Enemy patrol route and proximity chase (kinematic AI).
                    // Waypoints are world positions relative to the player's initial car spawn.
                    const Vector3 enemyOrigin=toRay(state.position);
                    const std::array<Vector3,6> patrol{{
                        Vector3{enemyOrigin.x+70,0.4f,enemyOrigin.z+75},
                        Vector3{enemyOrigin.x+95,0.4f,enemyOrigin.z+75},
                        Vector3{enemyOrigin.x+105,0.4f,enemyOrigin.z+45},
                        Vector3{enemyOrigin.x+90,0.4f,enemyOrigin.z+20},
                        Vector3{enemyOrigin.x+65,0.4f,enemyOrigin.z+25},
                        Vector3{enemyOrigin.x+55,0.4f,enemyOrigin.z+55}
                    }};
                    Vector3 enemyPosition=patrol[0];
                    float enemyYaw=0.0f;
                    float enemySpeed=0.0f;
                    float enemyReacquireCooldown=0.0f;
                    int enemyWaypoint=1;
                    bool enemyChasing=false;
                    bool localCirclePatrol=false;
                    constexpr int circlePoints=16;
                    constexpr float circleRadius=14.0f;
                    std::array<Vector3,circlePoints> circularPatrol{};
                    int circleWaypoint=1;
                    auto buildCirclePatrol=[&]() {
                        // The first point is exactly where the pursuit ended.
                        // Offset the center to the enemy's right, keeping the
                        // circle tangent to its current direction of travel.
                        const float centerX=enemyPosition.x+
                            std::cos(enemyYaw)*circleRadius;
                        const float centerZ=enemyPosition.z-
                            std::sin(enemyYaw)*circleRadius;
                        const float startAngle=std::atan2(
                            enemyPosition.z-centerZ,enemyPosition.x-centerX);
                        constexpr float tau=6.2831853071795864769f;
                        for(int i=0;i<circlePoints;++i) {
                            const float a=startAngle+tau*static_cast<float>(i)/circlePoints;
                            circularPatrol[i]=Vector3{
                                centerX+std::cos(a)*circleRadius,
                                enemyPosition.y,
                                centerZ+std::sin(a)*circleRadius
                            };
                        }
                        circleWaypoint=1;
                        localCirclePatrol=true;
                    };
                    constexpr float enemyDetectRadius=22.0f;
                    constexpr float enemyLoseRadius=32.0f;
                    constexpr float enemyPatrolSpeed=7.0f;
                    constexpr float enemyChaseSpeed=13.0f;
                    constexpr float enemyTurnRate=2.2f;
                    constexpr float enemyCircleSpeed=5.5f;
                    auto normalizeAngle=[](float a) {
                        return std::atan2(std::sin(a),std::cos(a));
                    };

                    struct GrabBox {
                        Vector3 position;
                        Vector3 velocity{0,0,0};
                        float size=0.85f;
                        bool stored=false;
                    };
                    std::vector<GrabBox> grabBoxes;
                    // Spawn demo boxes on the ground near the car.
                    for(int i=0;i<12;++i) {
                        const int column=i%4, row=i/4;
                        grabBoxes.push_back(GrabBox{
                            Vector3{toRay(state.position).x+5.0f+column*1.8f,0.425f,toRay(state.position).z-2.0f+row*2.0f}
                        });
                    }
                    // Spatial inventory: 4 columns x 2 rows (8 cells).
                    // Item footprints: small 1x1, medium 2x1, large 2x2.
                    struct InventoryItem {
                        int crate=-1;
                        int x=0,y=0,w=1,h=1;
                        int quantity=1;
                        std::array<int,3> crates{{-1,-1,-1}};
                    };
                    std::vector<InventoryItem> inventory;
                    int selectedItem=-1;
                    bool rotateOnPickup=false;
                    bool inventoryOpen=true;
                    auto crateDimensions=[](int index) {
                        // Three item categories, determined by spawned crate ID.
                        if(index%6==0) return std::array<int,3>{2,2,1};
                        if(index%3==0) return std::array<int,3>{2,1,2};
                        return std::array<int,3>{1,1,3};
                    };
                    auto inventoryFits=[&](int x,int y,int w,int h,int ignore) {
                        if(x<0 || y<0 || x+w>4 || y+h>2) return false;
                        for(int i=0;i<static_cast<int>(inventory.size());++i) {
                            if(i==ignore) continue;
                            const InventoryItem& other=inventory[i];
                            if(x<other.x+other.w && x+w>other.x &&
                               y<other.y+other.h && y+h>other.y) return false;
                        }
                        return true;
                    };
                    enum class Mission { Available, Pickup, Delivering, Completed, Failed };
                    Mission mission=Mission::Available;
                    constexpr float missionLimit=480.0f;
                    float missionTime=missionLimit;
                    int missionReward=0;
                    float cargoCondition=100.0f;
                    float cargoImpactCooldown=0.0f;
                    float previousVerticalSpeed=0.0f;
                    float lastDeliveryDistance=65.0f;
                    unsigned int contractNumber=1;
                    // Deterministic PRNG, independent of the render frame rate.
                    unsigned int deliverySeed=0xA53C9E17u;
                    auto nextRandom=[&]() {
                        deliverySeed^=deliverySeed<<13;
                        deliverySeed^=deliverySeed>>17;
                        deliverySeed^=deliverySeed<<5;
                        return deliverySeed;
                    };
                    // Persistent wallet: plain-text save next to the executable's working directory.
                    // Only non-negative, bounded balances are accepted from disk.
                    constexpr int maxWallet=100000000;
                    int wallet=0;
                    float vehicleCondition=100.0f;
                    float collisionCooldown=0.0f;
                    constexpr int repairCostPerPoint=8;
                    int engineUpgrade=0, tireUpgrade=0, suspensionUpgrade=0;
                    int deliveryUpgrade=0; // 0..3, permanent payout upgrade
                    constexpr int upgradePrices[3]={600,1200,2400};
                    const char* walletSavePath="cargo_run_save.txt";
                    {
                        std::ifstream save(walletSavePath);
                        long long balance=-1;
                        if(save >> balance && balance>=0 && balance<=maxWallet)
                            wallet=static_cast<int>(balance);
                        int level=0;
                        if(save >> level && level>=0 && level<=3)
                            deliveryUpgrade=level;
                        int e=0,t=0,s=0;
                        if(save >> e >> t >> s && e>=0 && e<=3 &&
                           t>=0 && t<=3 && s>=0 && s<=3) {
                            engineUpgrade=e; tireUpgrade=t; suspensionUpgrade=s;
                        }
                        float savedCondition=100.0f;
                        if(save >> savedCondition && std::isfinite(savedCondition) &&
                           savedCondition>=0.0f && savedCondition<=100.0f)
                            vehicleCondition=savedCondition;
                    }
                    rv_vehicle_set_condition(car,vehicleCondition);
                    rv_vehicle_apply_upgrades(car,engineUpgrade,tireUpgrade,suspensionUpgrade);
                    const std::array<int,3> missionCrates{{1,2,4}};
                    const Vector3 garageZone{state.position.x,0.05f,state.position.z};
                    // Each completed contract activates a new green destination.
                    const std::array<Vector3,5> deliveryZones{{
                        {state.position.x+8.0f,0.05f,state.position.z+65.0f},
                        {state.position.x+105.0f,0.05f,state.position.z+35.0f},
                        {state.position.x-85.0f,0.05f,state.position.z+115.0f},
                        {state.position.x-120.0f,0.05f,state.position.z-75.0f},
                        {state.position.x+75.0f,0.05f,state.position.z-125.0f}
                    }};
                    std::size_t deliveryIndex=0;
                    Vector3 deliveryZone=deliveryZones[deliveryIndex];
                    auto nextDeliveryZone=[&](Vector3 from) {
                        constexpr float tau=6.2831853071795864769f;
                        const float angle=tau*static_cast<float>(nextRandom()%10000u)/10000.0f;
                        const float radius=65.0f+static_cast<float>(nextRandom()%116u);
                        Vector3 candidate{from.x+std::cos(angle)*radius,0.05f,
                                          from.z+std::sin(angle)*radius};
                        // Keep destinations within the flat 1500m demo terrain.
                        candidate.x=std::clamp(candidate.x,enemyOrigin.x-600.0f,
                                              enemyOrigin.x+600.0f);
                        candidate.z=std::clamp(candidate.z,enemyOrigin.z-600.0f,
                                              enemyOrigin.z+600.0f);
                        lastDeliveryDistance=std::hypot(candidate.x-from.x,
                                                        candidate.z-from.z);
                        return candidate;
                    };
                    auto missionCargo=[&]() {
                        int count=0;
                        for(int id:missionCrates) if(grabBoxes[id].stored) ++count;
                        return count;
                    };
                    auto removeMissionCargo=[&]() {
                        for(auto it=inventory.begin();it!=inventory.end();) {
                            for(int k=0;k<it->quantity;) {
                                bool required=false;
                                for(int id:missionCrates) if(it->crates[k]==id) required=true;
                                if(required) {
                                    for(int j=k;j+1<it->quantity;++j) it->crates[j]=it->crates[j+1];
                                    it->crates[--it->quantity]=-1;
                                } else ++k;
                            }
                            if(it->quantity==0) it=inventory.erase(it);
                            else ++it;
                        }
                        selectedItem=-1;
                    };
                    int grabbedBox=-1;
                    float grabDistance=5.0f;
                    Vector3 grabEnd{0,0,0};
                    bool showGrabRay=false;

                    while (!WindowShouldClose()) {
                        float frame=std::min(GetFrameTime(),0.1f);
                        accumulator+=frame;
                        // Garage shop: U purchases a permanent payout upgrade.
                        // Garage access is free: stop the car inside the marked zone
                        // or walk into it. Money is only required for purchases.
                        const RVBodyState garageCarState=rv_vehicle_get_state(car);
                        const Vector3 garagePlayerPosition=player.insideCar ?
                            toRay(garageCarState.position) : player.position();
                        const bool garageInRange=
                            std::hypot(garagePlayerPosition.x-garageZone.x,
                                       garagePlayerPosition.z-garageZone.z)<=12.0f;
                        const bool atGarage=garageInRange &&
                            (!player.insideCar || std::abs(garageCarState.speed_kmh)<2.0f);
                        const auto saveGarage=[&]() {
                            std::ofstream save(walletSavePath,std::ios::trunc);
                            if(save) save << wallet << '\n' << deliveryUpgrade << '\n'
                                          << engineUpgrade << ' ' << tireUpgrade << ' '
                                          << suspensionUpgrade << '\n'
                                          << vehicleCondition << '\n';
                        };
                        const auto purchaseUpgrade=[&](int& level,int price) {
                            if(!atGarage || level>=3 || wallet<price) return;
                            wallet-=price;
                            ++level;
                            rv_vehicle_apply_upgrades(car,engineUpgrade,tireUpgrade,suspensionUpgrade);
                            saveGarage();
                        };
                        const int repairCost=static_cast<int>(
                            std::ceil((100.0f-vehicleCondition)*repairCostPerPoint));
                        if(atGarage && IsKeyPressed(KEY_FOUR) &&
                           repairCost>0 && wallet>=repairCost) {
                            wallet-=repairCost;
                            vehicleCondition=100.0f;
                            rv_vehicle_set_condition(car,vehicleCondition);
                            saveGarage();
                        }
                        if(atGarage && IsKeyPressed(KEY_ONE))
                            purchaseUpgrade(engineUpgrade,800*(engineUpgrade+1));
                        if(atGarage && IsKeyPressed(KEY_TWO))
                            purchaseUpgrade(tireUpgrade,650*(tireUpgrade+1));
                        if(atGarage && IsKeyPressed(KEY_THREE))
                            purchaseUpgrade(suspensionUpgrade,700*(suspensionUpgrade+1));
                        if(atGarage && IsKeyPressed(KEY_U) && deliveryUpgrade<3 &&
                           wallet>=upgradePrices[deliveryUpgrade]) {
                            wallet-=upgradePrices[deliveryUpgrade];
                            ++deliveryUpgrade;
                            saveGarage();
                        }
                        if(IsKeyPressed(KEY_N) &&
                           (mission==Mission::Available || mission==Mission::Completed ||
                            mission==Mission::Failed)) {
                            removeMissionCargo();
                            for(int id:missionCrates) {
                                GrabBox& box=grabBoxes[id];
                                box.stored=false;
                                box.velocity=Vector3{0,0,0};
                                box.position=Vector3{
                                    state.position.x+5.0f+(id%4)*1.8f,0.425f,
                                    state.position.z-2.0f+(id/4)*2.0f};
                            }
                            mission=Mission::Pickup;
                            missionTime=missionLimit;
                            missionReward=0;
                            cargoCondition=100.0f;
                            cargoImpactCooldown=0.0f;
                            previousVerticalSpeed=0.0f;
                        }
                        if(mission==Mission::Pickup || mission==Mission::Delivering) {
                            missionTime=std::max(0.0f,missionTime-frame);
                            if(missionTime<=0.0f) mission=Mission::Failed;
                            else mission=missionCargo()==3?Mission::Delivering:Mission::Pickup;
                        }

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
                        updateCityScript(L,player.insideCar ? pos : player.position());
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

                        // AI state machine: PATROL -> CHASE -> PATROL with hysteresis.
                        // Kinematic steering follows the waypoint sequence.
                        {
                            const Vector3 targetCar=toRay(state.position);
                            const float distance=std::hypot(
                                targetCar.x-enemyPosition.x,targetCar.z-enemyPosition.z);
                            enemyReacquireCooldown=std::max(0.0f,
                                enemyReacquireCooldown-frame);
                            if(enemyChasing) {
                                if(distance>enemyLoseRadius) {
                                    enemyChasing=false;
                                    enemyReacquireCooldown=2.5f;
                                    buildCirclePatrol();
                                }
                            } else if(distance<enemyDetectRadius &&
                                      enemyReacquireCooldown<=0.0f) {
                                enemyChasing=true;
                            }
                            Vector3 target=enemyChasing?targetCar:
                                (localCirclePatrol?circularPatrol[circleWaypoint]:
                                                   patrol[enemyWaypoint]);
                            float dx=target.x-enemyPosition.x;
                            float dz=target.z-enemyPosition.z;
                            float targetDistance=std::hypot(dx,dz);
                            if(!enemyChasing && targetDistance<2.0f) {
                                if(localCirclePatrol) {
                                    circleWaypoint=(circleWaypoint+1)%circlePoints;
                                    target=circularPatrol[circleWaypoint];
                                } else {
                                    enemyWaypoint=(enemyWaypoint+1)%static_cast<int>(patrol.size());
                                    target=patrol[enemyWaypoint];
                                }
                                dx=target.x-enemyPosition.x;
                                dz=target.z-enemyPosition.z;
                                targetDistance=std::hypot(dx,dz);
                            }
                            if(targetDistance>0.05f) {
                                // Local model forward is +Z, matching raylib yaw.
                                const float desiredYaw=std::atan2(dx,dz);
                                const float delta=normalizeAngle(desiredYaw-enemyYaw);
                                enemyYaw+=std::clamp(delta,-enemyTurnRate*frame,
                                                         enemyTurnRate*frame);
                            }
                            const float targetSpeed=enemyChasing?enemyChaseSpeed:
                                (localCirclePatrol?enemyCircleSpeed:enemyPatrolSpeed);
                            const float turnPenalty=std::clamp(
                                1.0f-std::abs(normalizeAngle(
                                    std::atan2(dx,dz)-enemyYaw))/2.5f,0.25f,1.0f);
                            const float desiredSpeed=targetDistance<1.5f && enemyChasing
                                ?0.0f:targetSpeed*turnPenalty;
                            const float speedDelta=std::clamp(
                                desiredSpeed-enemySpeed,-12.0f*frame,8.0f*frame);
                            enemySpeed+=speedDelta;
                            enemyPosition.x+=std::sin(enemyYaw)*enemySpeed*frame;
                            enemyPosition.z+=std::cos(enemyYaw)*enemySpeed*frame;
                        }
                        // Enemy contact damage uses a cooldown to avoid per-frame hits.
                        collisionCooldown=std::max(0.0f,collisionCooldown-frame);
                        const float enemyGap=std::hypot(pos.x-enemyPosition.x,
                                                       pos.z-enemyPosition.z);
                        if(enemyGap<3.0f && collisionCooldown<=0.0f &&
                           enemySpeed>2.0f && vehicleCondition>0.0f) {
                            vehicleCondition=std::max(0.0f,vehicleCondition-
                                std::clamp(enemySpeed*1.5f,5.0f,20.0f));
                            rv_vehicle_set_condition(car,vehicleCondition);
                            if(mission==Mission::Delivering) {
                                cargoCondition=std::max(0.0f,cargoCondition-
                                    std::clamp(enemySpeed*1.25f,4.0f,18.0f));
                                cargoImpactCooldown=1.0f;
                            }
                            collisionCooldown=2.0f;
                            std::ofstream save(walletSavePath,std::ios::trunc);
                            if(save) save << wallet << '\n' << deliveryUpgrade << '\n'
                                          << engineUpgrade << ' ' << tireUpgrade << ' '
                                          << suspensionUpgrade << '\n'
                                          << vehicleCondition << '\n';
                        }

                        // Damage fragile cargo from abrupt vertical impacts.
                        // Only cargo loaded for an active delivery can be damaged.
                        cargoImpactCooldown=std::max(0.0f,cargoImpactCooldown-frame);
                        const float verticalSpeed=state.velocity.y;
                        if(mission==Mission::Delivering && missionCargo()==3 &&
                           cargoImpactCooldown<=0.0f) {
                            const float verticalDelta=std::abs(
                                verticalSpeed-previousVerticalSpeed);
                            if(verticalDelta>7.0f) {
                                cargoCondition=std::max(0.0f,cargoCondition-
                                    std::clamp((verticalDelta-7.0f)*2.5f,2.0f,20.0f));
                                cargoImpactCooldown=1.0f;
                            }
                        }
                        previousVerticalSpeed=verticalSpeed;
                        const float distanceToDelivery=std::hypot(
                            pos.x-deliveryZone.x,pos.z-deliveryZone.z);
                        if(mission==Mission::Delivering && missionCargo()==3 &&
                           distanceToDelivery<6.0f && player.insideCar &&
                           std::abs(state.speed_kmh)<5.0f) {
                            mission=Mission::Completed;
                            const int baseReward=450+
                                static_cast<int>(lastDeliveryDistance*3.0f)+
                                static_cast<int>(missionTime*0.15f);
                            const int upgradedReward=baseReward*
                                (100+deliveryUpgrade*10)/100;
                            // Cargo integrity scales the payout from 40% to 100%.
                            missionReward=static_cast<int>(upgradedReward*
                                (0.40f+0.006f*cargoCondition));
                            wallet=std::min(maxWallet,wallet+missionReward);
                            {
                                std::ofstream save(walletSavePath,std::ios::trunc);
                                if(save) save << wallet << '\n' << deliveryUpgrade << '\n'
                                              << engineUpgrade << ' ' << tireUpgrade << ' '
                                              << suspensionUpgrade << '\n'
                                              << vehicleCondition << '\n';
                                else std::fprintf(stderr,"Unable to save Cargo Run wallet\n");
                            }
                            removeMissionCargo();
                            for(int id:missionCrates) grabBoxes[id].stored=false;
                            deliveryZone=nextDeliveryZone(deliveryZone);
                            ++contractNumber;
                        }
                        // Right mouse button: cast a red ray and grab the nearest box.
                        // The ray is cast from the camera through the mouse cursor.
                        const Ray grabRay=GetScreenToWorldRay(GetMousePosition(),camera);
                        showGrabRay=IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && !player.insideCar;
                        if(showGrabRay && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                            grabbedBox=-1;
                            float nearest=25.0f;
                            for(int i=0;i<static_cast<int>(grabBoxes.size());++i) {
                                const GrabBox& box=grabBoxes[i];
                                if(box.stored) continue;
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
                        // TAB toggles the inventory panel; R rotates the selected
                        // item, Q stores a ray-held crate, G drops one crate.
                        if(IsKeyPressed(KEY_TAB)) inventoryOpen=!inventoryOpen;
                        if(IsKeyPressed(KEY_R) && !player.insideCar) {
                            if(inventoryOpen && selectedItem>=0 &&
                               selectedItem<static_cast<int>(inventory.size())) {
                                InventoryItem& item=inventory[selectedItem];
                                if(inventoryFits(item.x,item.y,item.h,item.w,selectedItem))
                                    std::swap(item.w,item.h);
                            } else if(grabbedBox>=0) rotateOnPickup=!rotateOnPickup;
                        }
                        if(!player.insideCar && IsKeyPressed(KEY_Q) && grabbedBox>=0) {
                            const auto dims=crateDimensions(grabbedBox);
                            bool stored=false;
                            // Stack identical footprint classes, up to capacity.
                            for(int i=0;i<static_cast<int>(inventory.size());++i) {
                                InventoryItem& item=inventory[i];
                                const auto kind=crateDimensions(item.crate);
                                if(kind==dims && item.quantity<dims[2]) {
                                    item.crates[item.quantity++]=grabbedBox;
                                    selectedItem=i;
                                    stored=true;
                                    break;
                                }
                            }
                            if(!stored) {
                                int w=dims[0],h=dims[1];
                                if(rotateOnPickup) std::swap(w,h);
                                for(int y=0;y<2 && !stored;++y) {
                                    for(int x=0;x<4 && !stored;++x) {
                                        if(inventoryFits(x,y,w,h,-1)) {
                                            InventoryItem item;
                                            item.crate=grabbedBox;
                                            item.crates[0]=grabbedBox;
                                            item.x=x;item.y=y;item.w=w;item.h=h;
                                            inventory.push_back(item);
                                            selectedItem=static_cast<int>(inventory.size())-1;
                                            stored=true;
                                        }
                                    }
                                }
                            }
                            if(stored) {
                                grabBoxes[grabbedBox].stored=true;
                                grabBoxes[grabbedBox].velocity=Vector3{0,0,0};
                                grabbedBox=-1;
                            }
                        }
                        if(!player.insideCar && IsKeyPressed(KEY_G) &&
                           selectedItem>=0 &&
                           selectedItem<static_cast<int>(inventory.size())) {
                            InventoryItem& item=inventory[selectedItem];
                            const int index=item.crates[--item.quantity];
                            item.crates[item.quantity]=-1;
                            if(item.quantity==0) {
                                inventory.erase(inventory.begin()+selectedItem);
                                selectedItem=inventory.empty()?-1:
                                    std::min(selectedItem,static_cast<int>(inventory.size())-1);
                            }
                            GrabBox& box=grabBoxes[index];
                            box.stored=false;
                            const Vector3 forward=Vector3Normalize(Vector3{
                                camera.target.x-camera.position.x,0,
                                camera.target.z-camera.position.z
                            });
                            box.position=Vector3Add(player.position(),
                                Vector3Add(Vector3Scale(forward,2.0f),
                                           Vector3{0,box.size*0.5f+0.3f,0}));
                            box.velocity=Vector3{0,0,0};
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
                            if(i==grabbedBox || grabBoxes[i].stored) continue;
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
                                if(a.stored) continue;
                                const float ha=a.size*0.5f;
                                if(a.position.y<ha) {
                                    a.position.y=ha;
                                    if(a.velocity.y<0) a.velocity.y=0;
                                }
                                for(int j=i+1;j<static_cast<int>(grabBoxes.size());++j) {
                                    GrabBox& b=grabBoxes[j];
                                    if(b.stored) continue;
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
                        if (IsShaderValid(cityFogShader)) BeginShaderMode(cityFogShader);
                        drawCity(camera);
                        if (IsShaderValid(cityFogShader)) EndShaderMode();
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
                        // Visible navigation route and enemy vehicle.
                        if(localCirclePatrol) {
                            for(int i=0;i<circlePoints;++i) {
                                const Vector3 a=circularPatrol[i];
                                const Vector3 b=circularPatrol[(i+1)%circlePoints];
                                DrawLine3D(Vector3{a.x,0.07f,a.z},
                                           Vector3{b.x,0.07f,b.z},SKYBLUE);
                                DrawSphere(Vector3{a.x,0.11f,a.z},0.18f,BLUE);
                            }
                        } else {
                            for(int i=0;i<static_cast<int>(patrol.size());++i) {
                                const Vector3 a=patrol[i];
                                const Vector3 b=patrol[(i+1)%patrol.size()];
                                DrawLine3D(Vector3{a.x,0.07f,a.z},
                                           Vector3{b.x,0.07f,b.z},SKYBLUE);
                                DrawSphere(Vector3{a.x,0.11f,a.z},0.22f,BLUE);
                            }
                        }
                        const Quaternion enemyRotation=QuaternionFromAxisAngle(
                            Vector3{0,1,0},enemyYaw);
                        chassis.draw(Vector3{enemyPosition.x,enemyPosition.y-0.30f,
                                              enemyPosition.z},
                                     enemyRotation,1.0f,enemyChasing?RED:ORANGE);
                        DrawSphere(Vector3{enemyPosition.x,enemyPosition.y+2.0f,
                                            enemyPosition.z},0.24f,
                                   enemyChasing?RED:YELLOW);

                        DrawCylinder(garageZone,12.0f,12.0f,0.06f,32,Fade(GOLD,0.22f));
                        DrawCylinderWires(garageZone,12.0f,12.0f,0.06f,32,GOLD);
                        DrawCylinder(deliveryZone,6.0f,6.0f,0.08f,32,Fade(GREEN,0.3f));
                        DrawCylinderWires(deliveryZone,6.0f,6.0f,0.08f,32,DARKGREEN);
                        DrawLine3D(deliveryZone,Vector3{deliveryZone.x,6.0f,deliveryZone.z},GREEN);
                        DrawSphere(Vector3{deliveryZone.x,6.0f,deliveryZone.z},0.45f,GREEN);
                        player.draw();
                        // Green fake contact shadow: footprint stays on the ground
                        // even when a crate is lifted by the red ray.
                        for(const GrabBox& box:grabBoxes) {
                            if(box.stored) continue;
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
                            if(box.stored) continue;
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
                            "ON FOOT: WASD move | SPACE jump | RMB grab | Q store | G drop",20,54,16,RAYWHITE);
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
                        // Spatial inventory: 4x2 cells, item size, rotation and stacks.
                        if(inventoryOpen) {
                            constexpr int cell=62;
                            const int left=GetScreenWidth()-4*cell-25;
                            const int top=GetScreenHeight()-2*cell-115;
                            DrawRectangle(left-12,top-46,4*cell+24,2*cell+94,
                                          Fade(BLACK,0.86f));
                            DrawText("INVENTORY 4x2",left,top-37,20,RAYWHITE);
                            const Vector2 mouse=GetMousePosition();
                            if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                                int clickedItem=-1;
                                for(int i=static_cast<int>(inventory.size())-1;i>=0;--i) {
                                    const InventoryItem& item=inventory[i];
                                    const Rectangle rect{
                                        float(left+item.x*cell),float(top+item.y*cell),
                                        float(item.w*cell),float(item.h*cell)};
                                    if(CheckCollisionPointRec(mouse,rect)) {
                                        clickedItem=i;break;
                                    }
                                }
                                if(clickedItem>=0) selectedItem=clickedItem;
                                else if(selectedItem>=0 &&
                                        selectedItem<static_cast<int>(inventory.size())) {
                                    const int gx=static_cast<int>(std::floor((mouse.x-left)/cell));
                                    const int gy=static_cast<int>(std::floor((mouse.y-top)/cell));
                                    InventoryItem& item=inventory[selectedItem];
                                    if(inventoryFits(gx,gy,item.w,item.h,selectedItem)) {
                                        item.x=gx;item.y=gy;
                                    }
                                }
                            }
                            for(int y=0;y<2;++y)
                                for(int x=0;x<4;++x)
                                    DrawRectangleLinesEx(Rectangle{
                                        float(left+x*cell),float(top+y*cell),
                                        float(cell),float(cell)},1.0f,GRAY);
                            for(int i=0;i<static_cast<int>(inventory.size());++i) {
                                const InventoryItem& item=inventory[i];
                                const int x=left+item.x*cell,y=top+item.y*cell;
                                const int w=item.w*cell,h=item.h*cell;
                                DrawRectangle(x+3,y+3,w-6,h-6,
                                              i==selectedItem?Color{125,90,50,255}:
                                              Color{90,65,45,255});
                                DrawRectangleLinesEx(Rectangle{
                                    float(x+3),float(y+3),float(w-6),float(h-6)},
                                    i==selectedItem?3.0f:1.0f,
                                    i==selectedItem?YELLOW:BEIGE);
                                DrawText("BOX",x+10,y+12,18,RAYWHITE);
                                DrawText(TextFormat("x%d",item.quantity),
                                         x+10,y+h-26,18,YELLOW);
                            }
                            int used=0,quantity=0;
                            for(const InventoryItem& item:inventory) {
                                used+=item.w*item.h;
                                quantity+=item.quantity;
                            }
                            DrawText(TextFormat("%d/8 cells | %d boxes",used,quantity),
                                     left,top+2*cell+7,16,RAYWHITE);
                            DrawText("Click select/place | R rotate | Q store | G drop | TAB hide",
                                     10,GetScreenHeight()-22,16,RAYWHITE);
                        } else {
                            DrawText("TAB: INVENTORY",10,GetScreenHeight()-24,16,RAYWHITE);
                        }
                        DrawText(TextFormat("ENEMY: %s | %.1f m | waypoint %d",
                            enemyChasing?"CHASE":(localCirclePatrol?"CIRCLE PATROL":"PATROL"),
                            std::hypot(pos.x-enemyPosition.x,pos.z-enemyPosition.z),
                            localCirclePatrol?circleWaypoint+1:enemyWaypoint+1),
                            20,224,18,enemyChasing?RED:SKYBLUE);
                        DrawRectangle(10,260,555,108,Fade(BLACK,0.78f));
                         DrawText(TextFormat("CARGO RUN - CONTRACT %03u | WALLET: $%d",
                                             contractNumber,wallet),
                                  20,268,20,GOLD);
                         const char* missionLabel=
                             mission==Mission::Available?"N: ACCEPT DELIVERY CONTRACT":
                             mission==Mission::Pickup?"COLLECT CRATES 1, 2, 4 (RMB + Q)":
                             mission==Mission::Delivering?"DRIVE TO GREEN MARKER AND STOP":
                             mission==Mission::Completed?"DELIVERY COMPLETE! NEW GREEN ZONE! N: NEXT":
                             "TIME EXPIRED! N: RETRY";
                         DrawText(missionLabel,20,294,18,RAYWHITE);
                         DrawText(TextFormat("Cargo %d/3 | Time %02d:%02d | Distance %.0f m",
                             missionCargo(),static_cast<int>(missionTime)/60,
                             static_cast<int>(missionTime)%60,distanceToDelivery),
                             20,318,18,SKYBLUE);
                         if(mission==Mission::Completed)
                             DrawText(TextFormat("REWARD: $%d",missionReward),20,342,18,GREEN);
                         else DrawText("N contract | Q store | F exit | TAB inventory",20,342,16,LIGHTGRAY);
                         DrawText(TextFormat("VEHICLE CONDITION: %.0f%% | POWER: %.0f%% | GRIP: %.0f%%",
                                             vehicleCondition,40.0f+0.6f*vehicleCondition,
                                             65.0f+0.35f*vehicleCondition),
                                  20,365,18,vehicleCondition>40.0f?GREEN:RED);
                         DrawText(TextFormat("CARGO CONDITION: %.0f%% | ROUTE: %.0f m | PAYOUT x%.0f%%",
                                             cargoCondition,lastDeliveryDistance,
                                             40.0f+0.6f*cargoCondition),
                                  20,387,18,cargoCondition>=70.0f?GREEN:ORANGE);
                         if(atGarage) {
                             DrawRectangle(10,415,620,170,Fade(BLACK,0.80f));
                             DrawText(TextFormat("GARAGE | Delivery bonus: +%d%%",deliveryUpgrade*10),
                                      20,422,20,GOLD);
                             DrawText(TextFormat("1 Engine +12%%: $%d (Lv %d/3)",
                                      engineUpgrade<3?800*(engineUpgrade+1):0,engineUpgrade),
                                      20,448,18,RAYWHITE);
                             DrawText(TextFormat("2 Tires +8%% grip: $%d (Lv %d/3)",
                                      tireUpgrade<3?650*(tireUpgrade+1):0,tireUpgrade),
                                      20,472,18,RAYWHITE);
                             DrawText(TextFormat("3 Suspension +8%%: $%d (Lv %d/3)",
                                      suspensionUpgrade<3?700*(suspensionUpgrade+1):0,suspensionUpgrade),
                                      20,496,18,RAYWHITE);
                             if(deliveryUpgrade<3)
                                 DrawText(TextFormat("U: Upgrade delivery bonus (+10%%) - $%d",
                                                   upgradePrices[deliveryUpgrade]),
                                          20,520,18,RAYWHITE);
                             else DrawText("All delivery upgrades purchased",20,520,18,GREEN);
                             DrawText(TextFormat("4 Repair vehicle: $%d (Condition %.0f%%)",
                                                 repairCost,vehicleCondition),
                                      20,544,18,vehicleCondition<100.0f?YELLOW:GREEN);
                         }
                         DrawFPS(GetScreenWidth()-100,20);
                        EndDrawing();
                    }
                    player.unload();
                }
            }
            lua_close(mainState);
            unloadCity();
        }
    }
    chassis.unload();
    if (pavementLoaded) {
        // UnloadModel owns its material textures, including pavement.
        UnloadModel(pavementModel);
    }
    if (IsShaderValid(cityFogShader)) UnloadShader(cityFogShader);
    CloseWindow();
    return status;
}
