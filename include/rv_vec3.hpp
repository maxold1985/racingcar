#pragma once

// C++ vector operators for the C-compatible RVVec3 POD type.
// Include rv_physics.h from C, include rv_vec3.hpp from C++.
#include "rv_physics.h"
#include <cmath>

inline constexpr RVVec3 operator+(RVVec3 a, RVVec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline constexpr RVVec3 operator-(RVVec3 a, RVVec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline constexpr RVVec3 operator-(RVVec3 v) noexcept {
    return {-v.x, -v.y, -v.z};
}
inline constexpr RVVec3 operator*(RVVec3 v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}
inline constexpr RVVec3 operator*(float s, RVVec3 v) noexcept {
    return v * s;
}
// Component-wise/Hadamard multiplication; dot product is rv_math::dot(a, b).
inline constexpr RVVec3 operator*(RVVec3 a, RVVec3 b) noexcept {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
inline constexpr RVVec3 operator/(RVVec3 v, float s) noexcept {
    return {v.x / s, v.y / s, v.z / s};
}
inline RVVec3& operator+=(RVVec3& a, RVVec3 b) noexcept {
    a = a + b; return a;
}
inline RVVec3& operator-=(RVVec3& a, RVVec3 b) noexcept {
    a = a - b; return a;
}
inline RVVec3& operator*=(RVVec3& v, float s) noexcept {
    v = v * s; return v;
}
inline RVVec3& operator/=(RVVec3& v, float s) noexcept {
    v = v / s; return v;
}

namespace rv_math {
inline constexpr float dot(RVVec3 a, RVVec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline constexpr RVVec3 cross(RVVec3 a, RVVec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(RVVec3 v) noexcept {
    return std::sqrt(dot(v, v));
}
inline RVVec3 normalize(RVVec3 v) noexcept {
    const float len = length(v);
    return len > 1e-6f ? v / len : RVVec3{0, 0, 0};
}
} // namespace rv_math
