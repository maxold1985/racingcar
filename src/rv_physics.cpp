#include "rv_vec3.hpp"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RV_EPS 1.0e-6f

typedef struct RVWheel {
    RVWheelDesc desc;
    RVWheelState state;
    RVVec3 ground_velocity;
} RVWheel;

struct RVVehicle {
    RVBodyDesc cfg;
    RVBodyState body;
    float throttle, brake, steer;
    RVWheel wheels[RV_MAX_WHEELS];
    int count;
    float stock_engine_force;
    float stock_friction[RV_MAX_WHEELS];
    float stock_spring[RV_MAX_WHEELS];
    float stock_bump[RV_MAX_WHEELS];
    float stock_rebound[RV_MAX_WHEELS];
};

static float clampf(float x, float low, float high) { return fmaxf(low, fminf(high, x)); }
static RVVec3 v3(float x, float y, float z) { RVVec3 v = {x,y,z}; return v; }
using rv_math::dot;
using rv_math::cross;
static float length3(RVVec3 v) { return rv_math::length(v); }
static RVVec3 norm(RVVec3 v) { return rv_math::normalize(v); }
static RVQuat quat_norm(RVQuat q) {
    float n = sqrtf(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if (n < RV_EPS || !isfinite(n)) { RVQuat ident={0,0,0,1}; return ident; }
    RVQuat o={q.x/n,q.y/n,q.z/n,q.w/n}; return o;
}
static RVQuat quat_mul(RVQuat a, RVQuat b) {
    RVVec3 av=v3(a.x,a.y,a.z), bv=v3(b.x,b.y,b.z);
    RVVec3 uv = bv * a.w + av * b.w + cross(av, bv);
    RVQuat result={uv.x,uv.y,uv.z,a.w*b.w-dot(av,bv)}; return result;
}
static RVVec3 rotate(RVQuat q, RVVec3 p) {
    RVVec3 u=v3(q.x,q.y,q.z);
    RVVec3 t=cross(u, p) * 2.0f;
    return p + t * q.w + cross(u, t);
}
static RVVec3 inv_rotate(RVQuat q, RVVec3 p) {
    RVQuat conj={-q.x,-q.y,-q.z,q.w}; return rotate(conj,p);
}
static RVVec3 point_speed(const RVBodyState* b, RVVec3 point) {
    return b->velocity + cross(b->angular_velocity, point - b->position);
}
static void add_at(RVVec3 force, RVVec3 at, RVVec3 center, RVVec3* total, RVVec3* torque) {
    *total += force;
    *torque += cross(at - center, force);
}

RVBodyDesc rv_default_body(void) {
    RVBodyDesc d;
    memset(&d,0,sizeof(d));
    d.mass=1200.0f;
    d.inertia=v3(2500.0f,2100.0f,600.0f);
    d.position=v3(0,1.0f,0);
    d.orientation=RVQuat{0,0,0,1};
    d.gravity=9.81f;
    d.linear_drag=0.45f;
    d.angular_drag=950.0f;
    d.max_steer_radians=0.5f;
    d.engine_force=5200.0f;
    d.brake_force=9500.0f;
    d.antiroll_front=9000.0f;
    d.antiroll_rear=7000.0f;
    return d;
}
RVWheelDesc rv_default_wheel(void) {
    RVWheelDesc d;
    memset(&d,0,sizeof(d));
    d.radius=0.34f;
    d.rest_length=0.55f;
    d.min_length=0.15f;
    d.max_length=0.82f;
    d.spring_stiffness=27000.0f;
    d.damper_bump=7000.0f;
    d.damper_rebound=8500.0f;
    d.max_suspension_force=20000.0f;
    d.friction=1.15f;
    d.lateral_stiffness=9000.0f;
    d.rolling_resistance=5.0f;
    d.axle=0;
    return d;
}

RVVehicle* rv_vehicle_create(const RVBodyDesc* desc) {
    if (!desc || !isfinite(desc->mass) || desc->mass<=0 ||
        desc->inertia.x<=0 || desc->inertia.y<=0 || desc->inertia.z<=0 ||
        !isfinite(desc->inertia.x) || !isfinite(desc->inertia.y) || !isfinite(desc->inertia.z)) return NULL;
    RVVehicle* v=(RVVehicle*)calloc(1,sizeof(RVVehicle));
    if (!v) return NULL;
    v->cfg=*desc;
    v->stock_engine_force=desc->engine_force;
    v->body.position=desc->position;
    v->body.orientation=quat_norm(desc->orientation);
    return v;
}
void rv_vehicle_destroy(RVVehicle* v) { free(v); }
int rv_vehicle_add_wheel(RVVehicle* v, const RVWheelDesc* d) {
    if (!v || !d || v->count>=RV_MAX_WHEELS || d->radius<=0 || d->rest_length<=0 ||
        d->min_length<0 || d->min_length>d->rest_length ||
        d->max_length<d->rest_length || d->spring_stiffness<0 ||
        d->damper_bump<0 || d->damper_rebound<0 || d->friction<0 ||
        d->max_suspension_force<0 || d->lateral_stiffness<0 || d->rolling_resistance<0 ||
        (d->axle!=0 && d->axle!=1) ||
        !isfinite(d->radius) || !isfinite(d->max_length)) return -1;
    int i=v->count++;
    v->wheels[i].desc=*d;
    v->stock_friction[i]=d->friction;
    v->stock_spring[i]=d->spring_stiffness;
    v->stock_bump[i]=d->damper_bump;
    v->stock_rebound[i]=d->damper_rebound;
    v->wheels[i].state.suspension_length=d->rest_length;
    return i;
}
int rv_vehicle_wheel_count(const RVVehicle* v) { return v ? v->count : 0; }
void rv_vehicle_set_pose(RVVehicle* v, RVVec3 p, RVQuat q) {
    if (v) { v->body.position=p; v->body.orientation=quat_norm(q); }
}
void rv_vehicle_set_velocity(RVVehicle* v, RVVec3 linear, RVVec3 angular) {
    if (v) { v->body.velocity=linear; v->body.angular_velocity=angular; }
}
void rv_vehicle_apply_upgrades(RVVehicle* v,int engine,int tires,int suspension) {
    if(!v || engine<0 || engine>3 || tires<0 || tires>3 ||
       suspension<0 || suspension>3) return;
    v->cfg.engine_force=v->stock_engine_force*(1.0f+0.12f*engine);
    for(int i=0;i<v->count;++i) {
        RVWheelDesc* d=&v->wheels[i].desc;
        d->friction=v->stock_friction[i]*(1.0f+0.08f*tires);
        d->spring_stiffness=v->stock_spring[i]*(1.0f+0.08f*suspension);
        d->damper_bump=v->stock_bump[i]*(1.0f+0.08f*suspension);
        d->damper_rebound=v->stock_rebound[i]*(1.0f+0.08f*suspension);
    }
}
void rv_vehicle_set_controls(RVVehicle* v, float throttle, float brake, float steer) {
    if (v) { v->throttle=clampf(throttle,-1,1); v->brake=clampf(brake,0,1); v->steer=clampf(steer,-1,1); }
}
RVBodyState rv_vehicle_get_state(const RVVehicle* v) {
    RVBodyState s; memset(&s,0,sizeof(s));
    if (v) s=v->body;
    return s;
}
RVWheelState rv_vehicle_get_wheel(const RVVehicle* v, int i) {
    RVWheelState s; memset(&s,0,sizeof(s));
    if (v && i>=0 && i<v->count) s=v->wheels[i].state;
    return s;
}

int rv_vehicle_step(RVVehicle* v, float dt, RVRaycastFn raycast, void* user) {
    if (!v || !raycast || !isfinite(dt) || dt<=0 || dt>1.0f/30.0f || v->count==0) return -1;
    RVBodyState* body=&v->body;
    RVVec3 up=norm(rotate(body->orientation,v3(0,1,0)));
    // Raycasts follow world gravity, not chassis roll/pitch: wheels must keep
    // searching for the floor when the body leans during steering.
    RVVec3 ground_up=v3(0,1,0);
    RVVec3 down=-ground_up;
    RVVec3 chassis_forward=norm(rotate(body->orientation,v3(0,0,1)));
    RVVec3 net= v3(0,-v->cfg.mass*v->cfg.gravity,0);
    RVVec3 torque=v3(0,0,0);
    float speed=length3(body->velocity);
    net += body->velocity * (-v->cfg.linear_drag * speed);
    torque -= body->angular_velocity * v->cfg.angular_drag;
    // Roll/pitch stabilization only while near the ground. A conventional
    // suspension's anti-roll bar alone cannot right a chassis that loses
    // wheel contact. This is a game-oriented upright assist.
    const RVVec3 tilt_axis=cross(up,ground_up);
    const float tilt_strength=18000.0f;
    const float tilt_damping=4500.0f;
    const RVVec3 tilt_rate=body->angular_velocity -
        ground_up*dot(body->angular_velocity,ground_up);
    torque += tilt_axis*tilt_strength - tilt_rate*tilt_damping;

    int driven_count=0;
    for (int i=0; i<v->count; ++i) if (v->wheels[i].desc.driven) ++driven_count;

    /* Pass 1: raycast and suspension. Damper uses instantaneous point velocity,
       avoiding numerical spikes from calculating (last_length - length)/dt. */
    for (int i=0; i<v->count; ++i) {
        RVWheel* w=&v->wheels[i];
        RVWheelDesc* d=&w->desc;
        RVWheelState* s=&w->state;
        RVVec3 mount=body->position + rotate(body->orientation, d->mount);
        s->grounded=0;
        w->ground_velocity=v3(0,0,0);
        s->suspension_force=0;
        s->compression=0;
        s->suspension_length=d->rest_length;
        s->lateral_speed=0;
        s->longitudinal_speed=0;
        s->steer_radians=d->steering ? v->steer*v->cfg.max_steer_radians : 0;
        s->contact_point=v3(0,0,0);
        s->contact_normal=v3(0,1,0);
        s->wheel_center=mount + down * d->rest_length;
        // Start the ray above the suspension anchor so a wheel can recover
        // after shallow ground penetration. The effective distance remains
        // relative to the original mount, not the elevated ray origin.
        const float recovery = d->max_length + d->radius;
        const RVVec3 ray_origin = mount + ground_up * recovery;
        const float ray_length = 2.0f * recovery;
        RVHit hit; memset(&hit,0,sizeof(hit));
        if (!raycast(user,ray_origin,down,ray_length,&hit) ||
            !isfinite(hit.distance) || hit.distance<0 || hit.distance>ray_length ||
            length3(hit.normal)<RV_EPS) continue;
        const float effective_distance = hit.distance - recovery;
        if (effective_distance > d->max_length+d->radius) continue;
        RVVec3 n=norm(hit.normal);
        if (dot(n,ground_up)<0.5f) continue;
        s->grounded=1;
        w->ground_velocity=hit.ground_velocity;
        s->contact_point=hit.point;
        s->contact_normal=n;
        s->suspension_length=clampf(effective_distance-d->radius,d->min_length,d->max_length);
        s->compression=fmaxf(0,d->rest_length-s->suspension_length);
        s->wheel_center=mount + down * s->suspension_length;
        RVVec3 pointvel=point_speed(body,mount) - hit.ground_velocity;
        float suspension_velocity=dot(pointvel,up);
        float damping=(suspension_velocity<0) ? d->damper_bump : d->damper_rebound;
        s->suspension_force=clampf(d->spring_stiffness*s->compression - damping*suspension_velocity,
                                   0,d->max_suspension_force);
    }

    /* Pass 2: antiroll by axle. Differential forces oppose chassis roll. */
    for (int i=0; i<v->count; ++i) for (int j=i+1; j<v->count; ++j) {
        RVWheel* a=&v->wheels[i]; RVWheel* b=&v->wheels[j];
        if (a->desc.axle!=b->desc.axle || a->desc.mount.x*b->desc.mount.x>=0 ||
            !a->state.grounded || !b->state.grounded) continue;
        float stiffness=a->desc.axle==0 ? v->cfg.antiroll_front : v->cfg.antiroll_rear;
        float transfer=stiffness*(a->state.compression-b->state.compression);
        a->state.suspension_force=clampf(a->state.suspension_force+transfer,0,a->desc.max_suspension_force);
        b->state.suspension_force=clampf(b->state.suspension_force-transfer,0,b->desc.max_suspension_force);
    }

    /* Pass 3: force application at suspension mounts and tire contact points. */
    for (int i=0; i<v->count; ++i) {
        RVWheel* w=&v->wheels[i];
        RVWheelDesc* d=&w->desc; RVWheelState* s=&w->state;
        if (!s->grounded) continue;
        RVVec3 mount=body->position + rotate(body->orientation, d->mount);
        RVVec3 susp_force=ground_up * s->suspension_force;
        add_at(susp_force,mount,body->position,&net,&torque);
        float a=s->steer_radians;
        RVVec3 raw_forward=chassis_forward * cosf(a) + rotate(body->orientation, v3(1,0,0)) * sinf(a);
        RVVec3 fwd=norm(raw_forward - s->contact_normal * dot(raw_forward, s->contact_normal));
        if (length3(fwd)<RV_EPS) continue;
        RVVec3 right=norm(cross(s->contact_normal,fwd));
        RVVec3 relative=point_speed(body, s->contact_point) - w->ground_velocity;
        s->longitudinal_speed=dot(relative,fwd);
        s->lateral_speed=dot(relative,right);
        float f_long= d->driven && driven_count>0 ? v->throttle*v->cfg.engine_force/(float)driven_count : 0;
        float brake_max=v->brake*v->cfg.brake_force/(float)v->count;
        float desired_brake=-s->longitudinal_speed*(v->cfg.mass/(float)v->count)/dt;
        f_long+=clampf(desired_brake,-brake_max,brake_max);
        f_long-=d->rolling_resistance*s->longitudinal_speed;
        float f_lat=-d->lateral_stiffness*s->lateral_speed;
        float normal_load=s->suspension_force*clampf(dot(ground_up,s->contact_normal),0,1);
        float max_grip=d->friction*normal_load;
        float magnitude=sqrtf(f_long*f_long+f_lat*f_lat);
        if (magnitude>max_grip && magnitude>RV_EPS) {
            float scale=max_grip/magnitude;
            f_long*=scale; f_lat*=scale;
        }
        add_at(fwd * f_long + right * f_lat,s->contact_point,body->position,&net,&torque);
        s->spin_radians+=s->longitudinal_speed*dt/d->radius;
    }

    // Arcade stability assist: damp sideways sliding and excess yaw only
    // while wheels are grounded. This preserves tire forces and avoids
    // changing the orientation directly (which causes visible snapping).
    int contacts=0;
    for(int i=0;i<v->count;++i) if(v->wheels[i].state.grounded) ++contacts;
    if(contacts>=2) {
        const RVVec3 horizontal_forward=norm(v3(chassis_forward.x,0,chassis_forward.z));
        const RVVec3 horizontal_right=v3(horizontal_forward.z,0,-horizontal_forward.x);
        const float forward_speed=dot(body->velocity,horizontal_forward);
        const float lateral_speed=dot(body->velocity,horizontal_right);
        // Countersteer/slip recovery is force-limited to available traction.
        const float grip_fraction=(float)contacts/(float)v->count;
        const float slip_accel=clampf(-lateral_speed*3.5f,
                                      -8.0f*grip_fraction,8.0f*grip_fraction);
        net += horizontal_right*(slip_accel*v->cfg.mass);
        // Yaw-rate target from bicycle kinematics, with steering reduced at
        // speed. Damping applies a torque, not an instantaneous yaw reset.
        const float speed_abs=fabsf(forward_speed);
        const float steer_factor=1.0f/(1.0f+speed_abs*0.055f);
        const float target_yaw=forward_speed*
            tanf(v->steer*v->cfg.max_steer_radians*steer_factor)/2.6f;
        const float yaw_error=body->angular_velocity.y-target_yaw;
        const float yaw_torque=clampf(-yaw_error*6500.0f,
                                      -14000.0f,14000.0f);
        torque.y+=yaw_torque*grip_fraction;
    }

    /* Semi-implicit Euler translation and quaternion orientation integration. */
    RVVec3 acceleration=net / v->cfg.mass;
    body->velocity += acceleration * dt;
    body->position += body->velocity * dt;
    /* Euler's rigid-body equation: tau = I*alpha + omega x (I*omega). */
    RVVec3 local_w=inv_rotate(body->orientation,body->angular_velocity);
    RVVec3 local_tau=inv_rotate(body->orientation,torque);
    RVVec3 Iw=v3(v->cfg.inertia.x*local_w.x,v->cfg.inertia.y*local_w.y,v->cfg.inertia.z*local_w.z);
    RVVec3 rhs=local_tau - cross(local_w, Iw);
    RVVec3 local_alpha=v3(rhs.x/v->cfg.inertia.x,rhs.y/v->cfg.inertia.y,rhs.z/v->cfg.inertia.z);
    body->angular_velocity += rotate(body->orientation, local_alpha) * dt;
    // Prevent an impulse from causing runaway angular velocity in the
    // simplified tire model. 3 rad/s is enough for responsive arcade turns.
    const float angular_speed=length3(body->angular_velocity);
    if (angular_speed>3.0f) body->angular_velocity *= 3.0f/angular_speed;
    RVQuat omega={body->angular_velocity.x,body->angular_velocity.y,body->angular_velocity.z,0};
    RVQuat qdot=quat_mul(omega,body->orientation);
    body->orientation=quat_norm(RVQuat{
        body->orientation.x+0.5f*dt*qdot.x,
        body->orientation.y+0.5f*dt*qdot.y,
        body->orientation.z+0.5f*dt*qdot.z,
        body->orientation.w+0.5f*dt*qdot.w
    });
    RVVec3 new_fwd=rotate(body->orientation,v3(0,0,1));
    body->speed_kmh=dot(body->velocity,new_fwd)*3.6f;
    return 0;
}
