#include "rv_luau.h"
#include <lua.h>
#include <lualib.h>
#include <cmath>
#include <cstdint>

RVLuauContext::~RVLuauContext() {
    for (auto& it : vehicles) rv_vehicle_destroy(it.second);
}

static RVLuauContext* ctx(lua_State* L) {
    return static_cast<RVLuauContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

static RVVehicle* check_vehicle(lua_State* L, RVLuauContext* c, int arg) {
    int id = static_cast<int>(luaL_checkinteger(L, arg));
    auto it = c->vehicles.find(id);
    if (it == c->vehicles.end()) {
        luaL_error(L, "invalid vehicle id: %d", id);
        return nullptr;
    }
    return it->second;
}

static float field_num(lua_State* L, int table, const char* name, float fallback) {
    lua_getfield(L, table, name);
    float value = fallback;
    if (!lua_isnoneornil(L, -1)) {
        if (!lua_isnumber(L, -1)) luaL_error(L, "expected number for field '%s'", name);
        value = static_cast<float>(lua_tonumber(L, -1));
        if (!std::isfinite(value)) luaL_error(L, "non-finite field '%s'", name);
    }
    lua_pop(L, 1);
    return value;
}
static bool field_bool(lua_State* L, int table, const char* name, bool fallback) {
    lua_getfield(L, table, name);
    bool value = lua_isnoneornil(L, -1) ? fallback : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return value;
}
static RVVec3 read_vec(lua_State* L, int index, RVVec3 initial) {
    // Luau native vectors support +, -, * and / in scripts. For compatibility,
    // also accept legacy {x=..., y=..., z=...} tables as inputs.
    if (lua_isvector(L, index)) {
        const float* components = lua_tovector(L, index);
        RVVec3 value{components[0], components[1], components[2]};
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
            luaL_error(L, "vector components must be finite");
        return value;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    return RVVec3{
        field_num(L, index, "x", initial.x),
        field_num(L, index, "y", initial.y),
        field_num(L, index, "z", initial.z)
    };
}
static RVVec3 field_vec(lua_State* L, int table, const char* name, RVVec3 initial) {
    lua_getfield(L, table, name);
    if (lua_isnoneornil(L, -1)) { lua_pop(L,1); return initial; }
    RVVec3 value = read_vec(L, lua_gettop(L), initial);
    lua_pop(L, 1);
    return value;
}
static RVQuat read_quat(lua_State* L, int table, RVQuat initial) {
    luaL_checktype(L, table, LUA_TTABLE);
    return RVQuat{
        field_num(L, table, "x", initial.x),
        field_num(L, table, "y", initial.y),
        field_num(L, table, "z", initial.z),
        field_num(L, table, "w", initial.w)
    };
}
static RVQuat field_quat(lua_State* L, int table, const char* name, RVQuat initial) {
    lua_getfield(L, table, name);
    if (lua_isnoneornil(L,-1)) { lua_pop(L,1); return initial; }
    RVQuat result=read_quat(L,lua_gettop(L),initial);
    lua_pop(L,1);
    return result;
}
static void push_vec(lua_State* L, RVVec3 v) {
#if LUA_VECTOR_SIZE == 4
    lua_pushvector(L, v.x, v.y, v.z, 0.0f);
#else
    lua_pushvector(L, v.x, v.y, v.z);
#endif
}
static void push_quat(lua_State* L, RVQuat q) {
    lua_createtable(L,0,4);
    lua_pushnumber(L,q.x); lua_setfield(L,-2,"x");
    lua_pushnumber(L,q.y); lua_setfield(L,-2,"y");
    lua_pushnumber(L,q.z); lua_setfield(L,-2,"z");
    lua_pushnumber(L,q.w); lua_setfield(L,-2,"w");
}
static void push_number(lua_State* L, const char* key, float value) {
    lua_pushnumber(L,value); lua_setfield(L,-2,key);
}
static void push_bool(lua_State* L, const char* key, bool value) {
    lua_pushboolean(L,value); lua_setfield(L,-2,key);
}

/* Physics.newVehicle({mass=..., inertia={...}, position={...}, engineForce=...}) */
static int l_new(lua_State* L) {
    RVLuauContext* c=ctx(L);
    luaL_checktype(L,1,LUA_TTABLE);
    RVBodyDesc b=rv_default_body();
    b.mass=field_num(L,1,"mass",b.mass);
    b.inertia=field_vec(L,1,"inertia",b.inertia);
    b.position=field_vec(L,1,"position",b.position);
    b.orientation=field_quat(L,1,"orientation",b.orientation);
    b.gravity=field_num(L,1,"gravity",b.gravity);
    b.linear_drag=field_num(L,1,"linearDrag",b.linear_drag);
    b.angular_drag=field_num(L,1,"angularDrag",b.angular_drag);
    b.max_steer_radians=field_num(L,1,"maxSteer",b.max_steer_radians);
    b.engine_force=field_num(L,1,"engineForce",b.engine_force);
    b.brake_force=field_num(L,1,"brakeForce",b.brake_force);
    b.antiroll_front=field_num(L,1,"antirollFront",b.antiroll_front);
    b.antiroll_rear=field_num(L,1,"antirollRear",b.antiroll_rear);
    if (b.gravity<0 || b.engine_force<0 || b.brake_force<0 || b.linear_drag<0 || b.angular_drag<0 ||
        b.antiroll_front<0 || b.antiroll_rear<0) { luaL_error(L,"negative physical parameter"); return 0; }
    RVVehicle* vehicle=rv_vehicle_create(&b);
    if (!vehicle) { luaL_error(L,"invalid body parameters or out of memory"); return 0; }
    const int id=c->next_id++;
    c->vehicles.emplace(id,vehicle);
    lua_pushinteger(L,id);
    return 1;
}
static int l_add_wheel(lua_State* L) {
    RVLuauContext* c=ctx(L);
    RVVehicle* v=check_vehicle(L,c,1);
    luaL_checktype(L,2,LUA_TTABLE);
    RVWheelDesc d=rv_default_wheel();
    d.mount=field_vec(L,2,"mount",d.mount);
    d.radius=field_num(L,2,"radius",d.radius);
    d.rest_length=field_num(L,2,"restLength",d.rest_length);
    d.min_length=field_num(L,2,"minLength",d.min_length);
    d.max_length=field_num(L,2,"maxLength",d.max_length);
    d.spring_stiffness=field_num(L,2,"spring",d.spring_stiffness);
    d.damper_bump=field_num(L,2,"bumpDamping",d.damper_bump);
    d.damper_rebound=field_num(L,2,"reboundDamping",d.damper_rebound);
    d.max_suspension_force=field_num(L,2,"maxSuspensionForce",d.max_suspension_force);
    d.friction=field_num(L,2,"friction",d.friction);
    d.lateral_stiffness=field_num(L,2,"lateralStiffness",d.lateral_stiffness);
    d.rolling_resistance=field_num(L,2,"rollingResistance",d.rolling_resistance);
    d.steering=field_bool(L,2,"steering",false) ? 1:0;
    d.driven=field_bool(L,2,"driven",false) ? 1:0;
    d.axle=static_cast<int>(field_num(L,2,"axle",d.axle));
    int index=rv_vehicle_add_wheel(v,&d);
    if (index<0) { luaL_error(L,"invalid wheel parameters or max 16 wheels"); return 0; }
    lua_pushinteger(L,index+1); /* Luau uses 1-based wheel indices */
    return 1;
}
static int l_pose(lua_State* L) {
    RVVehicle* v=check_vehicle(L,ctx(L),1);
    RVVec3 position=read_vec(L,2,RVVec3{0,0,0});
    RVQuat q=read_quat(L,3,RVQuat{0,0,0,1});
    rv_vehicle_set_pose(v,position,q); return 0;
}
static int l_velocity(lua_State* L) {
    RVVehicle* v=check_vehicle(L,ctx(L),1);
    RVVec3 linear=read_vec(L,2,RVVec3{0,0,0});
    RVVec3 angular=read_vec(L,3,RVVec3{0,0,0});
    rv_vehicle_set_velocity(v,linear,angular); return 0;
}
static int l_controls(lua_State* L) {
    RVVehicle* v=check_vehicle(L,ctx(L),1);
    float throttle=static_cast<float>(luaL_checknumber(L,2));
    float brake=static_cast<float>(luaL_checknumber(L,3));
    float steer=static_cast<float>(luaL_checknumber(L,4));
    if (!std::isfinite(throttle) || !std::isfinite(brake) || !std::isfinite(steer))
        { luaL_error(L,"controls must be finite"); return 0; }
    rv_vehicle_set_controls(v,throttle,brake,steer); return 0;
}
static int l_step(lua_State* L) {
    RVLuauContext* c=ctx(L);
    RVVehicle* v=check_vehicle(L,c,1);
    float dt=static_cast<float>(luaL_checknumber(L,2));
    if (!c->raycast) { luaL_error(L,"host raycast not registered"); return 0; }
    if (rv_vehicle_step(v,dt,c->raycast,c->raycast_user)!=0)
        { luaL_error(L,"invalid physics dt, vehicle or missing wheels (use dt <= 1/30)"); return 0; }
    return 0;
}
static int l_state(lua_State* L) {
    RVVehicle* v=check_vehicle(L,ctx(L),1);
    RVBodyState s=rv_vehicle_get_state(v);
    lua_createtable(L,0,5);
    push_vec(L,s.position); lua_setfield(L,-2,"position");
    push_quat(L,s.orientation); lua_setfield(L,-2,"orientation");
    push_vec(L,s.velocity); lua_setfield(L,-2,"velocity");
    push_vec(L,s.angular_velocity); lua_setfield(L,-2,"angularVelocity");
    push_number(L,"speedKmh",s.speed_kmh);
    return 1;
}
static int l_wheel(lua_State* L) {
    RVVehicle* v=check_vehicle(L,ctx(L),1);
    int index=static_cast<int>(luaL_checkinteger(L,2))-1;
    if (index<0 || index>=rv_vehicle_wheel_count(v)) { luaL_error(L,"wheel index out of bounds"); return 0; }
    RVWheelState s=rv_vehicle_get_wheel(v,index);
    lua_createtable(L,0,11);
    push_bool(L,"grounded",s.grounded!=0);
    push_number(L,"length",s.suspension_length);
    push_number(L,"compression",s.compression);
    push_number(L,"force",s.suspension_force);
    push_number(L,"lateralSpeed",s.lateral_speed);
    push_number(L,"longitudinalSpeed",s.longitudinal_speed);
    push_number(L,"steerAngle",s.steer_radians);
    push_number(L,"spinAngle",s.spin_radians);
    push_vec(L,s.wheel_center); lua_setfield(L,-2,"center");
    push_vec(L,s.contact_point); lua_setfield(L,-2,"contactPoint");
    push_vec(L,s.contact_normal); lua_setfield(L,-2,"normal");
    return 1;
}
static int l_destroy(lua_State* L) {
    RVLuauContext* c=ctx(L);
    int id=static_cast<int>(luaL_checkinteger(L,1));
    auto it=c->vehicles.find(id);
    if (it!=c->vehicles.end()) {
        rv_vehicle_destroy(it->second);
        c->vehicles.erase(it);
    }
    return 0;
}

void rv_register_luau(lua_State* L, RVLuauContext* context) {
    lua_createtable(L,0,9);
    struct Item { const char* name; lua_CFunction f; };
    const Item functions[]={
        {"newVehicle",l_new}, {"addWheel",l_add_wheel},
        {"setPose",l_pose}, {"setVelocity",l_velocity},
        {"setControls",l_controls}, {"step",l_step},
        {"getState",l_state}, {"getWheel",l_wheel}, {"destroy",l_destroy}
    };
    for (const Item& item : functions) {
        lua_pushlightuserdata(L,context);
        lua_pushcclosure(L,item.f,item.name,1);
        lua_setfield(L,-2,item.name);
    }
    lua_setglobal(L,"Physics");
}
