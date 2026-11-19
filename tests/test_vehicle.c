#include "rv_physics.h"
#ifdef NDEBUG
#undef NDEBUG // Keep runtime checks active in Release builds.
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int floor_y0(void* user, RVVec3 origin, RVVec3 direction, float max_distance, RVHit* hit) {
    (void)user;
    if (direction.y>=-1.0e-5f) return 0;
    float t=-origin.y/direction.y;
    if (t<0 || t>max_distance) return 0;
    hit->distance=t;
    hit->point=(RVVec3){origin.x+direction.x*t,origin.y+direction.y*t,origin.z+direction.z*t};
    hit->normal=(RVVec3){0,1,0};
    hit->ground_velocity=(RVVec3){0,0,0};
    return 1;
}
static RVVehicle* test_car(void) {
    RVBodyDesc body=rv_default_body();
    body.position=(RVVec3){0,0.96f,0};
    RVVehicle* car=rv_vehicle_create(&body);
    assert(car);
    for (int axle=0;axle<2;axle++) for (int side=-1;side<=1;side+=2) {
        RVWheelDesc w=rv_default_wheel();
        w.axle=axle;
        w.mount=(RVVec3){side*0.8f,-0.2f,axle==0 ? 1.3f : -1.3f};
        w.steering=axle==0;
        w.driven=axle==1;
        assert(rv_vehicle_add_wheel(car,&w)>=0);
    }
    return car;
}
int main(void) {
    RVVehicle* v=test_car();
    assert(rv_vehicle_wheel_count(v)==4);
    for (int i=0;i<500;i++) assert(rv_vehicle_step(v,1.0f/120.0f,floor_y0,NULL)==0);
    RVBodyState stationary=rv_vehicle_get_state(v);
    printf("idle height %.4f, vy %.5f, roll qw %.5f\n",stationary.position.y,stationary.velocity.y,stationary.orientation.w);
    assert(stationary.position.y>0.80f && stationary.position.y<1.12f);
    assert(fabsf(stationary.velocity.y)<0.1f);
    assert(rv_vehicle_get_wheel(v,0).grounded);
    rv_vehicle_set_controls(v,1,0,0);
    for (int i=0;i<360;i++) assert(rv_vehicle_step(v,1.0f/120.0f,floor_y0,NULL)==0);
    RVBodyState moving=rv_vehicle_get_state(v);
    printf("after throttle speed %.2f km/h posZ %.2f\n",moving.speed_kmh,moving.position.z);
    assert(moving.speed_kmh>10);
    assert(moving.position.z>1);
    rv_vehicle_set_controls(v,0,1,0);
    for (int i=0;i<350;i++) assert(rv_vehicle_step(v,1.0f/120.0f,floor_y0,NULL)==0);
    RVBodyState stopped=rv_vehicle_get_state(v);
    printf("after brake speed %.2f km/h\n",stopped.speed_kmh);
    assert(fabsf(stopped.speed_kmh)<3);
    assert(rv_vehicle_step(v,0.5f,floor_y0,NULL)==-1);
    rv_vehicle_destroy(v);
    puts("PASS: suspension + accel + brake + invalid step");
    return 0;
}
