#include "rv_luau.h"
#include <lua.h>
#include <lualib.h>
#include <luacode.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static int raycast_floor(void*, RVVec3 origin, RVVec3 direction, float max_dist, RVHit* hit) {
    if (direction.y >= -1.e-6f) return 0;
    float t = -origin.y / direction.y;
    if (t < 0 || t > max_dist) return 0;
    hit->distance = t;
    hit->point = RVVec3{origin.x + direction.x*t, origin.y + direction.y*t,
                        origin.z + direction.z*t};
    hit->normal = RVVec3{0, 1, 0};
    hit->ground_velocity = RVVec3{0, 0, 0};
    return 1;
}
static bool run_luau_file(lua_State* L, const std::string& path) {
    std::ifstream file(path);
    if (!file) { std::fprintf(stderr,"Can't read: %s\n",path.c_str()); return false; }
    std::ostringstream buffer; buffer << file.rdbuf();
    std::string source=buffer.str();
    size_t size=0;
    char* bytecode=luau_compile(source.data(),source.size(),nullptr,&size);
    if (!bytecode) { std::fprintf(stderr,"Luau compiler failed\n"); return false; }
    int status=luau_load(L,path.c_str(),bytecode,size,0);
    std::free(bytecode);
    if (status!=0) {
        std::fprintf(stderr,"Luau load: %s\n",lua_tostring(L,-1)); lua_pop(L,1); return false;
    }
    status=lua_pcall(L,0,0,0);
    if (status!=0) {
        std::fprintf(stderr,"Luau script: %s\n",lua_tostring(L,-1)); lua_pop(L,1); return false;
    }
    return true;
}
#ifndef RV_DEFAULT_SCRIPT_PATH
#define RV_DEFAULT_SCRIPT_PATH "scripts/race.luau"
#endif
int main(int argc,char** argv) {
    const char* path=argc>1 ? argv[1] : RV_DEFAULT_SCRIPT_PATH;
    RVLuauContext context;
    context.raycast=raycast_floor;
    lua_State* L=luaL_newstate();
    if (!L) { std::fprintf(stderr,"Luau VM allocation failed\n"); return 1; }
    luaL_openlibs(L);
    rv_register_luau(L,&context);
    luaL_sandbox(L);
    lua_State* script=lua_newthread(L); // remains rooted on the parent state's stack
    luaL_sandboxthread(script);
    if (!run_luau_file(script,path)) { lua_close(L); return 1; }

    constexpr float fixedDt=1.0f/120.0f;
    for (int i=0;i<1200;++i) { // 10 simulated seconds at 120 Hz
        lua_getglobal(script,"fixedUpdate");
        if (!lua_isfunction(script,-1)) {
            std::fprintf(stderr,"Your script must define fixedUpdate(dt)\n");
            lua_pop(script,1); lua_close(L); return 1;
        }
        lua_pushnumber(script,fixedDt);
        if (lua_pcall(script,1,0,0)!=0) {
            std::fprintf(stderr,"Luau tick #%d: %s\n",i,lua_tostring(script,-1));
            lua_pop(script,1); lua_close(L); return 1;
        }
    }
    std::puts("Simulation finished (10s / 120 Hz).");
    lua_close(L);
    return 0;
}
