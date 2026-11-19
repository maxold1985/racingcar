#pragma once
#include "raylib.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// CPU-skinned Mixamo character; FBX animation clips can be separate files.
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
    struct Weight { unsigned int vertex; float weight; };
    struct Bone { std::string name; aiMatrix4x4 offset; std::vector<Weight> weights; };
    struct Part {
        Model model{};
        std::vector<aiVector3D> bindVertices;
        std::vector<Bone> bones;
    };
    struct Clip {
        std::unique_ptr<Assimp::Importer> importer;
        const aiAnimation* animation=nullptr;
        double duration=0.0;
        double ticksPerSecond=25.0;
    };
    std::unique_ptr<Assimp::Importer> character_;
    const aiScene* scene_=nullptr;
    std::vector<Part> parts_;
    std::unordered_map<std::string,Clip> clips_;
    std::string active_;
    double time_=0.0;
    bool loop_=true, finished_=false, modelValid_=false;
    float scale_=1.0f;
    aiVector3D center_{0,0,0};
    void evaluate(const aiNode* node, const aiMatrix4x4& parent,
                  const aiAnimation* animation, double tick,
                  std::unordered_map<std::string,aiMatrix4x4>& transforms) const;
};
