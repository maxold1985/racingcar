#include "rv_model.hpp"
#include "raymath.h"
#include <cstdio>

bool RVModel::load(const std::string& path) {
    unload();
    if (path.empty() || !FileExists(path.c_str())) return false;
    const char* ext = GetFileExtension(path.c_str());
    if (!IsFileExtension(path.c_str(), ".glb;.gltf;.obj;.iqm")) {
        std::fprintf(stderr, "Unsupported model extension: %s\n", ext);
        return false;
    }
    model = LoadModel(path.c_str());
    active = IsModelValid(model);
    if (!active) std::fprintf(stderr, "Failed to load model: %s\n", path.c_str());
    return active;
}
void RVModel::makeCube(float width, float height, float length) {
    unload();
    model = LoadModelFromMesh(GenMeshCube(width, height, length));
    active = IsModelValid(model);
}
void RVModel::makeWheel(float radius, float width) {
    unload();
    // Cylinder's default axis is Y. Rotate 90 degrees around Z in the draw code.
    model = LoadModelFromMesh(GenMeshCylinder(radius, width, 16));
    active = IsModelValid(model);
}
void RVModel::unload() {
    if (active) UnloadModel(model);
    model = Model{};
    active = false;
}
void RVModel::draw(Vector3 position, Quaternion rotation, float scale, Color tint) const {
    if (!active) return;
    Vector3 axis{0,1,0};
    float angle = 0;
    QuaternionToAxisAngle(QuaternionNormalize(rotation), &axis, &angle);
    DrawModelEx(model, position, axis, angle * RAD2DEG,
                Vector3{scale, scale, scale}, tint);
}
