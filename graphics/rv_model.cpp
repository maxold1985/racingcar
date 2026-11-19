#include "rv_model.hpp"
#include "raymath.h"
#include <cstdio>
#include <algorithm>
#include <cmath>

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
    if (active) {
        // raylib's GLB/glTF loader imports material parameters and textures.
        // Do not overwrite model.materials or model.meshMaterial: DrawModelEx
        // uses them to render the correct material for each mesh.
        std::fprintf(stderr, "Imported 3D model: %d meshes, %d materials\n",
                     model.meshCount, model.materialCount);
        for (int i = 0; i < model.materialCount; ++i) {
            const Material& material = model.materials[i];
            const int albedo = MATERIAL_MAP_ALBEDO;
            const int normal = MATERIAL_MAP_NORMAL;
            const int metallic = MATERIAL_MAP_METALNESS;
            const int roughness = MATERIAL_MAP_ROUGHNESS;
            const int emission = MATERIAL_MAP_EMISSION;
            const Color base = material.maps[albedo].color;
            std::fprintf(stderr,
                         "  Material %d: baseColor RGBA(%u,%u,%u,%u) | "
                         "albedo texture %u | normal %u | metallic %u | roughness %u | emissive %u\n",
                         i, (unsigned)base.r, (unsigned)base.g,
                         (unsigned)base.b, (unsigned)base.a,
                         material.maps[albedo].texture.id,
                         material.maps[normal].texture.id,
                         material.maps[metallic].texture.id,
                         material.maps[roughness].texture.id,
                         material.maps[emission].texture.id);
        }
        const BoundingBox box = GetModelBoundingBox(model);
        const Vector3 size = Vector3Subtract(box.max, box.min);
        const float longest = std::max({size.x, size.y, size.z});
        if (std::isfinite(longest) && longest > 1e-5f) {
            modelScale = 3.5f / longest;
            collisionHalfExtents_ = Vector3Scale(size,0.5f*modelScale);
            modelCenter = Vector3Scale(Vector3Add(box.min, box.max), 0.5f);
            std::fprintf(stderr, "Model dimensions: %.3f x %.3f x %.3f, scale %.6f\n",
                         size.x, size.y, size.z, modelScale);
        }
    } else std::fprintf(stderr, "Failed to load model: %s\n", path.c_str());
    return active;
}
void RVModel::makeCube(float width, float height, float length) {
    unload();
    model = LoadModelFromMesh(GenMeshCube(width, height, length));
    active = IsModelValid(model);
    collisionHalfExtents_ = Vector3{width*0.5f,height*0.5f,length*0.5f};
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
    modelScale = 1.0f;
    modelCenter = Vector3{0,0,0};
    collisionHalfExtents_ = Vector3{0.85f,0.75f,1.75f};
}
void RVModel::draw(Vector3 position, Quaternion rotation, float scale, Color tint) const {
    if (!active) return;
    Vector3 axis{0,1,0};
    float angle = 0;
    QuaternionToAxisAngle(QuaternionNormalize(rotation), &axis, &angle);
    const float finalScale = scale * modelScale;
    const Vector3 centerOffset = Vector3RotateByQuaternion(
        Vector3Scale(modelCenter, finalScale), rotation);
    const Vector3 drawPosition = Vector3Subtract(position, centerOffset);
    DrawModelEx(model, drawPosition, axis, angle * RAD2DEG,
                Vector3{finalScale, finalScale, finalScale}, tint);
}
