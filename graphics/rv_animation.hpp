#pragma once
#include "raylib.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Mixamo FBX/GLB skeletal character. Clips are imported independently and
// retargeted through their own hierarchy onto the character's bind skeleton.
class RVAnimation {
public:
    bool loadCharacter(const char* path);
    bool loadClip(const std::string& name, const char* path);
    bool play(const std::string& name, bool loop);
    void update(float dt);
    void draw(Vector3 position, float yaw) const;
    void unload();
    bool loaded() const { return modelValid_; }
    bool finished() const { return finished_; }
    ~RVAnimation() { unload(); }
private:
    using Pose = std::unordered_map<std::string, aiMatrix4x4>;
    struct Weight { unsigned int vertex; float weight; };
    struct Bone {
        std::string name;
        aiMatrix4x4 targetBind;
        aiMatrix4x4 targetBindInverse;
        std::vector<Weight> weights;
    };
    struct Part {
        Model model{};
        std::vector<aiVector3D> bindVertices; // model-root coordinates, never modified
        std::vector<aiVector3D> bindNormals;
        std::vector<aiVector3D> skinnedVertices;
        std::vector<aiVector3D> skinnedNormals;
        std::vector<float> weightSums;
        std::vector<Bone> bones;
    };
    struct Clip {
        std::unique_ptr<Assimp::Importer> importer;
        const aiScene* source=nullptr;        // owner: importer or character_
        const aiAnimation* animation=nullptr;
        std::unordered_map<std::string, const aiNodeAnim*> channels;
        Pose sourceBind;                     // source's own rest hierarchy
        aiMatrix4x4 sourceRootInverse;
        double duration=0.0;
        double ticksPerSecond=25.0;
    };
    std::unique_ptr<Assimp::Importer> character_;
    const aiScene* scene_=nullptr;
    std::vector<Part> parts_;
    Pose targetBind_;
    std::unordered_map<std::string,Clip> clips_;
    std::string active_;
    double time_=0.0;
    bool loop_=true, finished_=false, modelValid_=false;
    bool warnedInvalidPose_=false;
    float scale_=1.0f;
    aiVector3D center_{0,0,0};
};
