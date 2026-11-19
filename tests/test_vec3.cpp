#include "rv_vec3.hpp"
#ifdef NDEBUG
#undef NDEBUG // Keep runtime checks active in Release builds.
#endif
#include <cassert>
#include <cmath>
#include <type_traits>

static_assert(sizeof(RVVec3) == sizeof(float) * 3, "RVVec3 C ABI must not change");
static_assert(std::is_standard_layout<RVVec3>::value, "RVVec3 must stay standard-layout");
static_assert(std::is_trivially_copyable<RVVec3>::value, "RVVec3 must stay trivially copyable");
constexpr RVVec3 testAdd = RVVec3{1, 2, 3} + RVVec3{4, 5, 6};
static_assert(testAdd.x == 5 && testAdd.y == 7 && testAdd.z == 9, "operator+");
constexpr RVVec3 testMultiply = RVVec3{2, 3, 4} * 2.0f;
static_assert(testMultiply.x == 4 && testMultiply.y == 6 && testMultiply.z == 8, "operator*");

static bool equal(RVVec3 a, RVVec3 b) {
    return std::fabs(a.x-b.x)<1e-5f && std::fabs(a.y-b.y)<1e-5f && std::fabs(a.z-b.z)<1e-5f;
}
int main() {
    RVVec3 a{1, 2, 3}, b{4, 5, 6};
    assert(equal(a+b, RVVec3{5,7,9}));
    assert(equal(a-b, RVVec3{-3,-3,-3}));
    assert(equal(-a, RVVec3{-1,-2,-3}));
    assert(equal(a*3.0f, RVVec3{3,6,9}));
    assert(equal(3.0f*a, RVVec3{3,6,9}));
    assert(equal(a*b, RVVec3{4,10,18}));
    assert(equal(b/2.0f, RVVec3{2,2.5f,3}));
    RVVec3 c=a; c+=b; assert(equal(c,RVVec3{5,7,9}));
    c-=b; assert(equal(c,a));
    c*=2.0f; assert(equal(c,RVVec3{2,4,6}));
    c/=2.0f; assert(equal(c,a));
    assert(rv_math::dot(a,b)==32.0f);
    assert(equal(rv_math::cross(RVVec3{1,0,0},RVVec3{0,1,0}),RVVec3{0,0,1}));
    assert(std::fabs(rv_math::length(RVVec3{3,4,0})-5.0f)<1e-5f);
    assert(equal(rv_math::normalize(RVVec3{0,0,0}),RVVec3{0,0,0}));
}
