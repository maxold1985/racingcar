#ifndef RV_PHYSICS_H
#define RV_PHYSICS_H

/* C11-compatible raycast vehicle library. Coordinates: +X right, +Y up, +Z forward.
   SI units: metres, seconds, kilograms, newtons, radians. */
#ifdef _WIN32
  #ifdef RV_BUILD_DLL
    #define RV_API __declspec(dllexport)
  #elif defined(RV_USE_DLL)
    #define RV_API __declspec(dllimport)
  #else
    #define RV_API
  #endif
#else
  #define RV_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RV_MAX_WHEELS 16

typedef struct RVVec3 { float x, y, z; } RVVec3;
typedef struct RVQuat { float x, y, z, w; } RVQuat;
typedef struct RVVehicle RVVehicle;

typedef struct RVBodyDesc {
    float mass;                 /* kg, >0 */
    RVVec3 inertia;             /* local diagonal inertia, kg*m^2, all >0 */
    RVVec3 position;
    RVQuat orientation;         /* world-from-local; automatically normalized */
    float gravity;              /* positive magnitude in m/s^2; 9.81 default */
    float linear_drag;          /* N/(m/s)^2, quadratic */
    float angular_drag;         /* N*m/(rad/s), linear */
    float max_steer_radians;    /* at full steering input */
    float engine_force;         /* N total across driven wheels */
    float brake_force;          /* N total across all wheels */
    float antiroll_front;       /* N/m, axle=0 */
    float antiroll_rear;        /* N/m, axle=1 */
} RVBodyDesc;

typedef struct RVWheelDesc {
    RVVec3 mount;               /* wheel suspension anchor, local chassis coords */
    float radius;
    float rest_length;
    float min_length;
    float max_length;
    float spring_stiffness;     /* N/m */
    float damper_bump;          /* N*s/m (compressing) */
    float damper_rebound;       /* N*s/m (extending) */
    float max_suspension_force; /* N, force cap */
    float friction;             /* coefficient of tire friction */
    float lateral_stiffness;    /* N/(m/s) */
    float rolling_resistance;   /* N/(m/s) */
    int steering;               /* 0/1 */
    int driven;                 /* 0/1 */
    int axle;                   /* 0 front, 1 rear; used by antiroll */
} RVWheelDesc;

typedef struct RVHit {
    float distance;             /* ray length from origin to surface, metres */
    RVVec3 point;               /* contact point in world coords */
    RVVec3 normal;              /* surface normal pointing out of ground */
    RVVec3 ground_velocity;     /* world m/s; zero for static ground */
} RVHit;

/* Return nonzero for hit; hit distance must be in [0, max_distance].
   Called once per wheel per fixed step; ignore vehicle chassis in your host query. */
typedef int (*RVRaycastFn)(void* user, RVVec3 origin, RVVec3 direction,
                           float max_distance, RVHit* hit);

typedef struct RVBodyState {
    RVVec3 position;
    RVQuat orientation;
    RVVec3 velocity;
    RVVec3 angular_velocity;    /* world rad/s */
    float speed_kmh;            /* signed, + forward */
} RVBodyState;

typedef struct RVWheelState {
    int grounded;
    float suspension_length;    /* metres */
    float compression;          /* metres; >=0 */
    float suspension_force;     /* newtons */
    float lateral_speed;        /* m/s */
    float longitudinal_speed;   /* m/s */
    float steer_radians;
    float spin_radians;         /* for wheel mesh animation */
    RVVec3 contact_point;
    RVVec3 contact_normal;
    RVVec3 wheel_center;
} RVWheelState;

RV_API RVBodyDesc rv_default_body(void);
RV_API RVWheelDesc rv_default_wheel(void);
RV_API RVVehicle* rv_vehicle_create(const RVBodyDesc* desc);
RV_API void rv_vehicle_destroy(RVVehicle* vehicle);
RV_API int rv_vehicle_add_wheel(RVVehicle* vehicle, const RVWheelDesc* desc);
RV_API int rv_vehicle_wheel_count(const RVVehicle* vehicle);
RV_API void rv_vehicle_set_pose(RVVehicle* vehicle, RVVec3 pos, RVQuat quat);
RV_API void rv_vehicle_set_velocity(RVVehicle* vehicle, RVVec3 linear, RVVec3 angular);
RV_API void rv_vehicle_set_controls(RVVehicle* vehicle, float throttle, float brake, float steer);
RV_API RVBodyState rv_vehicle_get_state(const RVVehicle* vehicle);
RV_API RVWheelState rv_vehicle_get_wheel(const RVVehicle* vehicle, int index);
/* Return 0 on success, -1 for invalid args; time step should be fixed (e.g. 1/120). */
RV_API int rv_vehicle_step(RVVehicle* vehicle, float dt, RVRaycastFn raycast, void* user);

#ifdef __cplusplus
}
#endif
#endif
