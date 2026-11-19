#include "rv_animation.hpp"
#include "raymath.h"
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/texture.h>
#include <cstdint>
#include <assimp/vector3.inl>
#include <assimp/matrix4x4.inl>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <unordered_set>
#include <utility>

namespace {
constexpr unsigned importFlags =
    aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
    aiProcess_LimitBoneWeights | aiProcess_GenSmoothNormals;

using Pose = std::unordered_map<std::string, aiMatrix4x4>;
using Channels = std::unordered_map<std::string, const aiNodeAnim*>;

std::string boneKey(std::string name) {
    // Preserve Assimp's auxiliary FBX nodes; strip only Mixamo namespaces.
    const auto pos=name.find_last_of(":|");
    return pos==std::string::npos?name:name.substr(pos+1);
}
bool finite(const aiVector3D& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
aiVector3D normalized(aiVector3D v) {
    const float length=v.Length();
    return length>1e-7f && std::isfinite(length)?v/length:aiVector3D(0,1,0);
}
aiVector3D direction(const aiMatrix4x4& m, const aiVector3D& v) {
    return aiVector3D(m.a1*v.x+m.a2*v.y+m.a3*v.z,
                      m.b1*v.x+m.b2*v.y+m.b3*v.z,
                      m.c1*v.x+m.c2*v.y+m.c3*v.z);
}
aiVector3D samplePosition(const aiNodeAnim* c,double t,const aiVector3D& base) {
    if(!c || !c->mNumPositionKeys) return base;
    if(c->mNumPositionKeys==1 || t<=c->mPositionKeys[0].mTime)
        return c->mPositionKeys[0].mValue;
    for(unsigned i=0;i+1<c->mNumPositionKeys;++i) {
        const auto& a=c->mPositionKeys[i];const auto& b=c->mPositionKeys[i+1];
        if(t<=b.mTime) {
            const float alpha=float((t-a.mTime)/std::max(1e-9,b.mTime-a.mTime));
            return a.mValue*(1-alpha)+b.mValue*alpha;
        }
    }
    return c->mPositionKeys[c->mNumPositionKeys-1].mValue;
}
aiQuaternion sampleRotation(const aiNodeAnim* c,double t,const aiQuaternion& base) {
    if(!c || !c->mNumRotationKeys) return base;
    if(c->mNumRotationKeys==1 || t<=c->mRotationKeys[0].mTime)
        return c->mRotationKeys[0].mValue;
    for(unsigned i=0;i+1<c->mNumRotationKeys;++i) {
        const auto& a=c->mRotationKeys[i];const auto& b=c->mRotationKeys[i+1];
        if(t<=b.mTime) {
            aiQuaternion q;
            aiQuaternion::Interpolate(q,a.mValue,b.mValue,
                float((t-a.mTime)/std::max(1e-9,b.mTime-a.mTime)));
            return q.Normalize();
        }
    }
    return c->mRotationKeys[c->mNumRotationKeys-1].mValue;
}
aiVector3D sampleScale(const aiNodeAnim* c,double t,const aiVector3D& base) {
    if(!c || !c->mNumScalingKeys) return base;
    if(c->mNumScalingKeys==1 || t<=c->mScalingKeys[0].mTime)
        return c->mScalingKeys[0].mValue;
    for(unsigned i=0;i+1<c->mNumScalingKeys;++i) {
        const auto& a=c->mScalingKeys[i];const auto& b=c->mScalingKeys[i+1];
        if(t<=b.mTime) {
            const float alpha=float((t-a.mTime)/std::max(1e-9,b.mTime-a.mTime));
            return a.mValue*(1-alpha)+b.mValue*alpha;
        }
    }
    return c->mScalingKeys[c->mNumScalingKeys-1].mValue;
}
void gatherPose(const aiNode* node, const aiMatrix4x4& parent,
                const Channels& channels, double tick, Pose& out,
                std::unordered_map<unsigned,aiMatrix4x4>* meshNodes=nullptr) {
    aiMatrix4x4 local=node->mTransformation;
    auto channel=channels.find(node->mName.C_Str());
    if(channel!=channels.end()) {
        aiVector3D scale, position;
        aiQuaternion rotation;
        local.Decompose(scale,rotation,position);
        const aiNodeAnim* c=channel->second;
        // With FBX pivot preservation disabled, Assimp collapses pivot
        // helper transforms into the imported node animation channels.
        // Every channel supplies its complete local TRS; do not suppress
        // translations based on bone names or helper-node heuristics.
        const aiVector3D p=samplePosition(c,tick,position);
        const aiQuaternion q=sampleRotation(c,tick,rotation);
        const aiVector3D vscale=sampleScale(c,tick,scale);
        aiMatrix4x4 scaling(vscale.x,0,0,0, 0,vscale.y,0,0,
                            0,0,vscale.z,0, 0,0,0,1);
        local=aiMatrix4x4(q.GetMatrix())*scaling;
        local.a4=p.x;
        local.b4=p.y;
        local.c4=p.z;
    }
    const aiMatrix4x4 world=parent*local;
    // Auxiliary FBX nodes retain unique keys (e.g. Hips_$AssimpFbx$_Rotation).
    out[boneKey(node->mName.C_Str())]=world;
    if(meshNodes) {
        for(unsigned i=0;i<node->mNumMeshes;++i)
            (*meshNodes)[node->mMeshes[i]]=world;
    }
    for(unsigned i=0;i<node->mNumChildren;++i)
        gatherPose(node->mChildren[i],world,channels,tick,out,meshNodes);
}
const aiAnimation* selectAnimation(const aiScene* scene) {
    if(!scene) return nullptr;
    const aiAnimation* best=nullptr;
    for(unsigned i=0;i<scene->mNumAnimations;++i) {
        const aiAnimation* candidate=scene->mAnimations[i];
        if(!candidate || !candidate->mNumChannels) continue;
        // Mixamo FBX files can contain an empty "Take 001" stack before
        // the populated "mixamo.com" take. Prefer real animated channels.
        if(!best || candidate->mNumChannels>best->mNumChannels ||
           (candidate->mNumChannels==best->mNumChannels &&
            candidate->mDuration>best->mDuration))
            best=candidate;
    }
    return best;
}
// Measure whether the FBX mesh-space inverse bind matches the imported
// rest hierarchy using affine error. Translation is measured in FBX units.
float bindMatrixError(const aiMatrix4x4& m) {
    return std::abs(m.a1-1)+std::abs(m.b2-1)+std::abs(m.c3-1)+
           std::abs(m.a2)+std::abs(m.a3)+std::abs(m.b1)+
           std::abs(m.b3)+std::abs(m.c1)+std::abs(m.c2)+
           std::abs(m.a4)+std::abs(m.b4)+std::abs(m.c4);
}
// Convert Assimp material textures into raylib textures. The importer owns
// embedded image bytes; raylib owns the uploaded GPU textures.
Texture2D loadAssimpTexture(const aiScene* scene,
                            const std::filesystem::path& modelPath,
                            const aiString& reference) {
    const std::string name=reference.C_Str();
    const aiTexture* embedded=scene->GetEmbeddedTexture(name.c_str());
    if(embedded) {
        Image image{};
        if(embedded->mHeight==0) {
            // Compressed embedded image (PNG/JPG/etc.).
            const char* hint=embedded->achFormatHint;
            std::string ext=(hint && hint[0])?std::string(".")+hint:".png";
            image=LoadImageFromMemory(ext.c_str(),
                reinterpret_cast<const unsigned char*>(embedded->pcData),
                static_cast<int>(embedded->mWidth));
        } else {
            // Uncompressed aiTexel is BGRA; convert to RGBA explicitly.
            const size_t count=size_t(embedded->mWidth)*embedded->mHeight;
            if(count>0 && count<=size_t(INT32_MAX)/4) {
                unsigned char* rgba=(unsigned char*)MemAlloc(count*4);
                for(size_t i=0;i<count;++i) {
                    const aiTexel& p=embedded->pcData[i];
                    rgba[4*i]=p.r;rgba[4*i+1]=p.g;
                    rgba[4*i+2]=p.b;rgba[4*i+3]=p.a;
                }
                image.data=rgba;
                image.width=int(embedded->mWidth);
                image.height=int(embedded->mHeight);
                image.mipmaps=1;
                image.format=PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            }
        }
        if(!IsImageValid(image)) return {};
        Texture2D texture=LoadTextureFromImage(image);
        UnloadImage(image);
        return texture;
    }
    if(name.empty() || name[0]=='*') return {};
    // FBX exporters sometimes store Windows absolute paths. Fall back to
    // the filename next to the FBX when that original path is unavailable.
    std::string normalized=name;
    std::replace(normalized.begin(),normalized.end(),'\\','/');
    const std::filesystem::path referenced=std::filesystem::u8path(normalized);
    const auto relative=modelPath.parent_path()/referenced;
    const auto adjacent=modelPath.parent_path()/referenced.filename();
    for(const auto& candidate:{referenced,relative,adjacent}) {
        const std::string path=candidate.u8string();
        if(FileExists(path.c_str())) {
            Texture2D texture=LoadTexture(path.c_str());
            if(IsTextureValid(texture)) return texture;
        }
    }
    return {};
}
Channels getChannels(const aiAnimation* a) {
    Channels channels;
    if(a) for(unsigned i=0;i<a->mNumChannels;++i)
        channels[a->mChannels[i]->mNodeName.C_Str()]=a->mChannels[i];
    return channels;
}
}

bool RVAnimation::loadCharacter(const char* path) {
    unload();
    if(!path || !path[0]) return false;
    character_=std::make_unique<Assimp::Importer>();
    // Ask Assimp to resolve FBX pre/post rotations and pivots during import.
    // Keeping helper nodes led to inconsistent animated joint transforms.
    character_->SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS,false);
    scene_=character_->ReadFile(path,importFlags);
    if(!scene_ || !scene_->mRootNode || !scene_->HasMeshes()) {
        std::fprintf(stderr,"Assimp character import failed: %s (%s)\n",
                     path,character_->GetErrorString());
        unload();
        return false;
    }
    characterPath_=path;
    std::unordered_map<unsigned,aiMatrix4x4> meshNodes;
    gatherPose(scene_->mRootNode,aiMatrix4x4(),Channels{},0.0,targetBind_,&meshNodes);
    const float extreme=std::numeric_limits<float>::max();
    aiVector3D low(extreme,extreme,extreme),high(-extreme,-extreme,-extreme);
    // Each Assimp mesh has its own material index. Cache GPU textures by
    // material so meshes sharing a material also share the same texture.
    std::vector<Material> importedMaterials;
    importedMaterials.reserve(scene_->mNumMaterials);
    for(unsigned i=0;i<scene_->mNumMaterials;++i) {
        const aiMaterial* source=scene_->mMaterials[i];
        Material material=LoadMaterialDefault();
        aiColor4D color(1,1,1,1);
        if(aiGetMaterialColor(source,AI_MATKEY_COLOR_DIFFUSE,&color)==AI_SUCCESS) {
            material.maps[MATERIAL_MAP_DIFFUSE].color=Color{
                (unsigned char)std::clamp(int(color.r*255),0,255),
                (unsigned char)std::clamp(int(color.g*255),0,255),
                (unsigned char)std::clamp(int(color.b*255),0,255),
                (unsigned char)std::clamp(int(color.a*255),0,255)};
        }
        aiString path;
        if(source->GetTexture(aiTextureType_BASE_COLOR,0,&path)!=AI_SUCCESS)
            source->GetTexture(aiTextureType_DIFFUSE,0,&path);
        if(path.length>0) {
            Texture2D texture=loadAssimpTexture(scene_,
                std::filesystem::u8path(characterPath_),path);
            if(IsTextureValid(texture)) {
                material.maps[MATERIAL_MAP_DIFFUSE].texture=texture;
                ownedTextures_.push_back(texture);
                std::fprintf(stderr,"FBX material %u: loaded albedo %s\n",
                             i,path.C_Str());
            } else {
                std::fprintf(stderr,"FBX material %u: missing albedo %s\n",
                             i,path.C_Str());
            }
        }
        importedMaterials.push_back(material);
    }
    unsigned totalBones=0, skippedBones=0, fallbackOffsets=0;
    for(unsigned mi=0;mi<scene_->mNumMeshes;++mi) {
        const aiMesh* src=scene_->mMeshes[mi];
        if(!src->mNumVertices || !src->mNumFaces) continue;
        if(src->mNumVertices>65535) {
            std::fprintf(stderr,"FBX part %u exceeds 65535 vertices; skipped\n",mi);
            continue;
        }
        Part part;
        const auto node=meshNodes.find(mi);
        const aiMatrix4x4 meshBind=node==meshNodes.end()?aiMatrix4x4():node->second;
        Mesh mesh{};
        mesh.vertexCount=int(src->mNumVertices);
        mesh.triangleCount=int(src->mNumFaces);
        mesh.vertices=(float*)MemAlloc(size_t(mesh.vertexCount)*3*sizeof(float));
        mesh.normals=(float*)MemAlloc(size_t(mesh.vertexCount)*3*sizeof(float));
        mesh.texcoords=(float*)MemAlloc(size_t(mesh.vertexCount)*2*sizeof(float));
        mesh.indices=(unsigned short*)MemAlloc(size_t(mesh.triangleCount)*3*sizeof(unsigned short));
        part.bindVertices.reserve(src->mNumVertices);
        part.bindNormals.reserve(src->mNumVertices);
        for(unsigned i=0;i<src->mNumVertices;++i) {
            // Bake the mesh-node transformation ONCE, for both static and
            // animated vertices. Never mix node-local and model-space inputs.
            const aiVector3D v=meshBind*src->mVertices[i];
            const aiVector3D rawNormal=src->HasNormals()?
                src->mNormals[i]:aiVector3D(0,1,0);
            const aiVector3D n=normalized(direction(meshBind,rawNormal));
            part.bindVertices.push_back(v);
            part.bindNormals.push_back(n);
            low.x=std::min(low.x,v.x);low.y=std::min(low.y,v.y);low.z=std::min(low.z,v.z);
            high.x=std::max(high.x,v.x);high.y=std::max(high.y,v.y);high.z=std::max(high.z,v.z);
            mesh.vertices[i*3]=v.x;mesh.vertices[i*3+1]=v.y;mesh.vertices[i*3+2]=v.z;
            mesh.normals[i*3]=n.x;mesh.normals[i*3+1]=n.y;mesh.normals[i*3+2]=n.z;
            const aiVector3D uv=src->HasTextureCoords(0)?
                src->mTextureCoords[0][i]:aiVector3D();
            mesh.texcoords[i*2]=uv.x;mesh.texcoords[i*2+1]=1.0f-uv.y;
        }
        for(unsigned i=0;i<src->mNumFaces;++i) {
            const aiFace& face=src->mFaces[i];
            if(face.mNumIndices!=3) {
                for(unsigned j=0;j<3;++j) mesh.indices[i*3+j]=0;
            } else for(unsigned j=0;j<3;++j)
                mesh.indices[i*3+j]=(unsigned short)face.mIndices[j];
        }
        for(unsigned b=0;b<src->mNumBones;++b) {
            const aiBone* input=src->mBones[b];
            ++totalBones;
            const std::string name=boneKey(input->mName.C_Str());
            auto found=targetBind_.find(name);
            if(found==targetBind_.end() ||
               std::abs(found->second.Determinant())<1e-12f) {
                ++skippedBones;
                continue;
            }
            Bone bone;
            bone.name=name;
            bone.targetBind=found->second;
            bone.targetBindInverse=found->second;
            bone.targetBindInverse.Inverse();
            // aiBone::mOffsetMatrix converts mesh-local bind vertices to
            // bone-local coordinates. Mesh vertices were already transformed
            // into model-root space with meshBind, so compensate precisely
            // once with inverse(meshBind).
            aiMatrix4x4 inverseMesh=meshBind;
            if(std::abs(inverseMesh.Determinant())>1e-12f) {
                inverseMesh.Inverse();
                bone.offsetInModelSpace=input->mOffsetMatrix*inverseMesh;
                // Some FBX exporters store non-bind node transforms. In
                // that case use the verified rest hierarchy instead, so
                // unanimated geometry is never stretched.
                const aiMatrix4x4 bindCheck=bone.targetBind*bone.offsetInModelSpace;
                if(!std::isfinite(bindMatrixError(bindCheck)) ||
                   bindMatrixError(bindCheck)>0.1f) {
                    bone.offsetInModelSpace=bone.targetBindInverse;
                    ++fallbackOffsets;
                }
            } else {
                bone.offsetInModelSpace=bone.targetBindInverse;
                ++fallbackOffsets;
            }
            for(unsigned w=0;w<input->mNumWeights;++w) {
                const auto& weight=input->mWeights[w];
                if(weight.mVertexId<src->mNumVertices && weight.mWeight>0)
                    bone.weights.push_back({weight.mVertexId,weight.mWeight});
            }
            part.bones.push_back(std::move(bone));
        }
        part.skinnedVertices.resize(part.bindVertices.size());
        part.skinnedNormals.resize(part.bindVertices.size());
        part.weightSums.resize(part.bindVertices.size());
        UploadMesh(&mesh,true);
        part.model=LoadModelFromMesh(mesh);
        if(src->mMaterialIndex<importedMaterials.size()) {
            // LoadModelFromMesh owns a default material. Replace its color
            // and albedo reference; texture lifetime is managed separately.
            Material& target=part.model.materials[0];
            const Material& source=importedMaterials[src->mMaterialIndex];
            target.maps[MATERIAL_MAP_DIFFUSE].color=
                source.maps[MATERIAL_MAP_DIFFUSE].color;
            target.maps[MATERIAL_MAP_DIFFUSE].texture=
                source.maps[MATERIAL_MAP_DIFFUSE].texture;
        }
        parts_.push_back(std::move(part));
    }
    // LoadMaterialDefault allocates only material metadata; free it without
    // unloading textures, which are owned by RVAnimation.
    for(Material& material:importedMaterials) {
        material.maps[MATERIAL_MAP_DIFFUSE].texture={};
        UnloadMaterial(material);
    }
    if(parts_.empty()) { unload();return false; }
    center_=aiVector3D((low.x+high.x)*0.5f,low.y,(low.z+high.z)*0.5f);
    const float height=high.y-low.y;
    scale_=(height>0.001f && std::isfinite(height))?1.75f/height:1.0f;
    modelValid_=true;
    if(const aiAnimation* animation=selectAnimation(scene_)) {
        Clip embedded;
        embedded.source=scene_;
        embedded.sameSkeleton=true;
        embedded.animation=animation;
        embedded.channels=getChannels(embedded.animation);
        embedded.sourceBind=targetBind_;
        for(const auto& kv:embedded.sourceBind) {
            if(std::abs(kv.second.Determinant())>1e-12f) {
                aiMatrix4x4 inverse=kv.second;
                inverse.Inverse();
                embedded.sourceBindInverse[kv.first]=inverse;
            }
        }
        embedded.duration=embedded.animation->mDuration;
        embedded.ticksPerSecond=embedded.animation->mTicksPerSecond>0?
            embedded.animation->mTicksPerSecond:25.0;
        clips_.emplace("Default",std::move(embedded));
    }
    std::fprintf(stderr,
        "Animated character: %u meshes, %zu skinned parts, %u bones (%u unmatched), scale %.5f\n",
        scene_->mNumMeshes,parts_.size(),totalBones,skippedBones,scale_);
    std::fprintf(stderr,"[FBX BIND] offsets requiring rest-pose fallback: %u/%u\n",
                 fallbackOffsets,totalBones);
    return true;
}
bool RVAnimation::loadClip(const std::string& name,const char* path) {
    if(!modelValid_ || !path || !path[0]) return false;
    // The same FBX is configured as character AND Walking. Reuse the
    // already-imported scene and its embedded animation, so the bone hierarchy,
    // FBX helper nodes and coordinate conversions are exactly identical.
    namespace fs=std::filesystem;
    std::error_code pathError;
    const bool sameFile=fs::equivalent(fs::u8path(characterPath_),
                                       fs::u8path(path),pathError);
    if(!pathError && sameFile && selectAnimation(scene_)) {
        Clip clip;
        clip.source=scene_;
        clip.animation=selectAnimation(scene_);
        clip.channels=getChannels(clip.animation);
        clip.sourceBind=targetBind_;
        for(const auto& kv:clip.sourceBind) {
            if(std::abs(kv.second.Determinant())>1e-12f) {
                aiMatrix4x4 inverse=kv.second;
                inverse.Inverse();
                clip.sourceBindInverse[kv.first]=inverse;
            }
        }
        clip.sameSkeleton=true;
        clip.duration=clip.animation->mDuration;
        clip.ticksPerSecond=clip.animation->mTicksPerSecond>0?
            clip.animation->mTicksPerSecond:25.0;
        const aiAnimation* selected=clip.animation;
        clips_[name]=std::move(clip);
        std::fprintf(stderr,
            "Animation %s: FBX take '%s' (%u channels, %.2f ticks) from character\n",
            name.c_str(),selected->mName.C_Str(),
            selected->mNumChannels,selected->mDuration);
        return true;
    }
    Clip clip;
    clip.importer=std::make_unique<Assimp::Importer>();
    clip.importer->SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS,false);
    clip.source=clip.importer->ReadFile(path,importFlags);
    if(!clip.source || !clip.source->mRootNode || !clip.source->HasAnimations()) {
        std::fprintf(stderr,"FBX animation import failed: %s (%s)\n",
                     path,clip.importer->GetErrorString());
        return false;
    }
    clip.animation=selectAnimation(clip.source);
    if(!clip.animation || clip.animation->mNumChannels==0) {
        std::fprintf(stderr,"Animation has no bone channels: %s\n",path);
        return false;
    }
    clip.duration=clip.animation->mDuration;
    clip.ticksPerSecond=clip.animation->mTicksPerSecond>0?
        clip.animation->mTicksPerSecond:25.0;
    clip.channels=getChannels(clip.animation);
    gatherPose(clip.source->mRootNode,aiMatrix4x4(),Channels{},0.0,clip.sourceBind);
    for(const auto& kv:clip.sourceBind) {
        if(std::abs(kv.second.Determinant())>1e-12f) {
            aiMatrix4x4 inverse=kv.second;
            inverse.Inverse();
            clip.sourceBindInverse[kv.first]=inverse;
        }
    }
    std::unordered_set<std::string> skeleton;
    for(const auto& part:parts_)
        for(const Bone& bone:part.bones) skeleton.insert(bone.name);
    unsigned matched=0;
    for(const auto& bone:skeleton)
        if(clip.sourceBindInverse.find(bone)!=clip.sourceBindInverse.end()) ++matched;
    // Reject incompatible rigs instead of warping every vertex unpredictably.
    if(!skeleton.empty() && matched*5<skeleton.size()*4) {
        std::fprintf(stderr,
            "Animation %s rejected: compatible bone names %u/%zu (needs >=80%%)\n",
            path,matched,skeleton.size());
        return false;
    }
    std::fprintf(stderr,"Animation %s loaded: %s (matching bones %u/%zu)\n",
                 name.c_str(),path,matched,skeleton.size());
    clips_[name]=std::move(clip);
    return true;
}
bool RVAnimation::play(const std::string& name,bool loop) {
    if(!modelValid_) return false;
    if(name=="BindPose") {
        if(active_=="BindPose") return true;
        for(Part& part:parts_) {
            Mesh& mesh=part.model.meshes[0];
            for(size_t i=0;i<part.bindVertices.size();++i) {
                const aiVector3D& v=part.bindVertices[i];
                const aiVector3D& n=part.bindNormals[i];
                mesh.vertices[3*i]=v.x;mesh.vertices[3*i+1]=v.y;mesh.vertices[3*i+2]=v.z;
                mesh.normals[3*i]=n.x;mesh.normals[3*i+1]=n.y;mesh.normals[3*i+2]=n.z;
            }
            UpdateMeshBuffer(mesh,0,mesh.vertices,int(part.bindVertices.size()*3*sizeof(float)),0);
            UpdateMeshBuffer(mesh,2,mesh.normals,int(part.bindNormals.size()*3*sizeof(float)),0);
        }
        active_="BindPose";loop_=true;time_=0;finished_=false;
        return true;
    }
    if(clips_.find(name)==clips_.end()) return false;
    if(active_==name && loop_==loop && !(finished_&&!loop)) return true;
    active_=name;loop_=loop;time_=0.0;finished_=false;
    return true;
}
void RVAnimation::update(float dt) {
    // Diagnostic: RV_BIND_POSE=1 leaves the uploaded meshes in their
    // original imported pose. If this still looks distorted, the problem
    // precedes animation (FBX node transforms / geometry import).
    const char* bindPose=std::getenv("RV_BIND_POSE");
    if(bindPose && std::strcmp(bindPose,"1")==0) return;
    if(!modelValid_ || active_.empty()) return;
    const auto it=clips_.find(active_);
    if(it==clips_.end()) return;
    const Clip& clip=it->second;
    time_+=std::max(0.0f,dt)*clip.ticksPerSecond;
    double tick=time_;
    if(clip.duration>0) {
        if(loop_) tick=std::fmod(tick,clip.duration);
        else if(tick>=clip.duration) {tick=clip.duration;finished_=true;}
    }
    Pose animated;
    gatherPose(clip.source->mRootNode,aiMatrix4x4(),clip.channels,tick,animated);
    // Character movement belongs to RVPlayer, not the animation clip.
    // Remove only horizontal displacement of Mixamo Hips relative to
    // its bind pose. Preserve vertical bobbing, limb movement and rotations.
    // Apply the correction to all animated global bone matrices so their
    // relative hierarchy remains unchanged.
    if(clip.sameSkeleton) {
        const auto hipsNow=animated.find("Hips");
        const auto hipsRest=targetBind_.find("Hips");
        if(hipsNow!=animated.end() && hipsRest!=targetBind_.end()) {
            const float dx=hipsNow->second.a4-hipsRest->second.a4;
            const float dz=hipsNow->second.c4-hipsRest->second.c4;
            for(auto& entry:animated) {
                entry.second.a4-=dx;
                entry.second.c4-=dz;
            }
        }
    }
    // One-time diagnostics: detect FBX channel translations/scales which
    // stretch bones away from their imported bind positions.
    static bool reportedPose=false;
    if(!reportedPose && clip.sameSkeleton) {
        reportedPose=true;
        struct Deviation { std::string name; float distance; float ratio; };
        std::vector<Deviation> deviations;
        for(const auto& [name, rest]:targetBind_) {
            auto it=animated.find(name);
            if(it==animated.end()) continue;
            const aiMatrix4x4& now=it->second;
            const aiVector3D a(rest.a4,rest.b4,rest.c4);
            const aiVector3D b(now.a4,now.b4,now.c4);
            const float dist=(b-a).Length();
            const float restScale=std::max(1e-6f,
                aiVector3D(rest.a1,rest.b1,rest.c1).Length());
            const float nowScale=aiVector3D(now.a1,now.b1,now.c1).Length();
            deviations.push_back({name,dist,nowScale/restScale});
        }
        std::sort(deviations.begin(),deviations.end(),
            [](const Deviation& a,const Deviation& b) {
                return a.distance>b.distance;
            });
        std::fprintf(stderr,"[FBX DIAG] tick=%.3f nodes=%zu\n",
                     tick,deviations.size());
        for(size_t i=0;i<std::min(size_t(12),deviations.size());++i)
            std::fprintf(stderr,"[FBX DIAG] %s translation=%.3f scaleRatio=%.3f\n",
                deviations[i].name.c_str(),deviations[i].distance,
                deviations[i].ratio);
    }
    // Compare anatomical joint-to-joint lengths throughout the FBX take.
    // Rigid bone rotations cannot change the distance between a joint and
    // its parent. Large changes identify broken FBX hierarchy evaluation,
    // unlike the normal displacement from T-pose to running.
    static bool reportedLengths=false;
    if(!reportedLengths && clip.sameSkeleton && clip.duration>0.0) {
        reportedLengths=true;
        std::unordered_set<std::string> skinnedNames;
        for(const Part& part:parts_)
            for(const Bone& bone:part.bones) skinnedNames.insert(bone.name);
        struct JointPair {
            std::string child,parent;
            float restLength;
        };
        std::vector<JointPair> pairs;
        const auto visit=[&](const auto& self,const aiNode* node,
                             const aiNode* nearestBone)->void {
            const std::string name=boneKey(node->mName.C_Str());
            const bool isBone=skinnedNames.count(name)!=0;
            if(isBone && nearestBone) {
                const std::string parentName=boneKey(nearestBone->mName.C_Str());
                const auto a=targetBind_.find(name);
                const auto b=targetBind_.find(parentName);
                if(a!=targetBind_.end() && b!=targetBind_.end()) {
                    const aiVector3D pa(a->second.a4,a->second.b4,a->second.c4);
                    const aiVector3D pb(b->second.a4,b->second.b4,b->second.c4);
                    const float length=(pa-pb).Length();
                    if(length>0.01f)
                        pairs.push_back({name,parentName,length});
                }
            }
            const aiNode* next=isBone?node:nearestBone;
            for(unsigned i=0;i<node->mNumChildren;++i)
                self(self,node->mChildren[i],next);
        };
        visit(visit,clip.source->mRootNode,nullptr);
        struct Distortion { std::string child,parent;float ratio,tick; };
        std::vector<Distortion> worst;
        for(const JointPair& pair:pairs) {
            float worstRatio=1.0f,worstTick=0.0f;
            // Sample all twenty frames of the short Mixamo clip.
            for(unsigned frame=0;frame<=20;++frame) {
                const double sampleTick=clip.duration*double(frame)/20.0;
                Pose sample;
                gatherPose(clip.source->mRootNode,aiMatrix4x4(),
                           clip.channels,sampleTick,sample);
                const auto a=sample.find(pair.child),b=sample.find(pair.parent);
                if(a==sample.end() || b==sample.end()) continue;
                const aiVector3D pa(a->second.a4,a->second.b4,a->second.c4);
                const aiVector3D pb(b->second.a4,b->second.b4,b->second.c4);
                const float ratio=(pa-pb).Length()/pair.restLength;
                if(std::isfinite(ratio) &&
                   std::abs(ratio-1.0f)>std::abs(worstRatio-1.0f)) {
                    worstRatio=ratio;worstTick=float(sampleTick);
                }
            }
            worst.push_back({pair.child,pair.parent,worstRatio,worstTick});
        }
        std::sort(worst.begin(),worst.end(),
                  [](const Distortion& a,const Distortion& b) {
                      return std::abs(a.ratio-1.0f)>std::abs(b.ratio-1.0f);
                  });
        std::fprintf(stderr,"[FBX LENGTH] checked %zu bone pairs over 21 samples\n",
                     pairs.size());
        for(size_t i=0;i<std::min(size_t(15),worst.size());++i)
            std::fprintf(stderr,
                "[FBX LENGTH] %s <- %s ratio=%.3f tick=%.2f\n",
                worst[i].child.c_str(),worst[i].parent.c_str(),
                worst[i].ratio,worst[i].tick);
    }
    bool invalid=false;
    for(Part& part:parts_) {
        if(part.bones.empty()) continue;
        std::fill(part.skinnedVertices.begin(),part.skinnedVertices.end(),aiVector3D());
        std::fill(part.skinnedNormals.begin(),part.skinnedNormals.end(),aiVector3D());
        std::fill(part.weightSums.begin(),part.weightSums.end(),0.0f);
        for(const Bone& bone:part.bones) {
            aiMatrix4x4 skin; // identity for missing bones
            const auto inverse=clip.sourceBindInverse.find(bone.name);
            const auto current=animated.find(bone.name);
            if(inverse!=clip.sourceBindInverse.end() && current!=animated.end()) {
                // Retarget each joint from its SOURCE rest pose to the
                // CHARACTER rest pose. At rest this product is exactly I.
                //
                // T = B_target * inv(B_source) * A_source(t) * inv(B_target)
                //
                // Use root/model-space bind vertices for every mesh.
                if(clip.sameSkeleton) {
                    // Model-root animated global transform times mesh-space
                    // inverse bind. Frame zero can be a running pose.
                    skin=current->second*bone.offsetInModelSpace;
                } else {
                    // Different FBX: source motion delta conjugated into the
                    // target character's bind coordinate system.
                    skin=bone.targetBind*inverse->second*current->second*
                         bone.targetBindInverse;
                }
            }
            for(const Weight& w:bone.weights) {
                if(w.vertex>=part.bindVertices.size()) continue;
                part.skinnedVertices[w.vertex]+=
                    (skin*part.bindVertices[w.vertex])*w.weight;
                part.skinnedNormals[w.vertex]+=
                    direction(skin,part.bindNormals[w.vertex])*w.weight;
                part.weightSums[w.vertex]+=w.weight;
            }
        }
        Mesh& mesh=part.model.meshes[0];
        for(size_t i=0;i<part.bindVertices.size();++i) {
            aiVector3D v=part.bindVertices[i],n=part.bindNormals[i];
            if(part.weightSums[i]>1e-6f) {
                v=part.skinnedVertices[i]/part.weightSums[i];
                n=normalized(part.skinnedNormals[i]);
            }
            if(!finite(v) || !finite(n)) {
                invalid=true;
                v=part.bindVertices[i];
                n=part.bindNormals[i];
            }
            mesh.vertices[i*3]=v.x;mesh.vertices[i*3+1]=v.y;mesh.vertices[i*3+2]=v.z;
            mesh.normals[i*3]=n.x;mesh.normals[i*3+1]=n.y;mesh.normals[i*3+2]=n.z;
        }
        UpdateMeshBuffer(mesh,0,mesh.vertices,int(part.bindVertices.size()*3*sizeof(float)),0);
        UpdateMeshBuffer(mesh,2,mesh.normals,int(part.bindNormals.size()*3*sizeof(float)),0);
    }
    if(invalid && !warnedInvalidPose_) {
        std::fprintf(stderr,"Animation produced non-finite coordinates; affected vertices reset to bind pose\n");
        warnedInvalidPose_=true;
    }
}
void RVAnimation::draw(Vector3 position,float yaw) const {
    if(!modelValid_) return;
    const Vector3 offset=Vector3RotateByAxisAngle(
        Vector3{-center_.x*scale_,-center_.y*scale_,-center_.z*scale_},
        Vector3{0,1,0},yaw);
    const Vector3 origin=Vector3Add(position,offset);
    for(const Part& part:parts_)
        DrawModelEx(part.model,origin,Vector3{0,1,0},yaw*RAD2DEG,
                    Vector3{scale_,scale_,scale_},WHITE);
}
void RVAnimation::unload() {
    for(Part& part:parts_) {
        // Prevent UnloadModel from unloading shared albedo textures.
        for(int i=0;i<part.model.materialCount;++i)
            part.model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture={};
        UnloadModel(part.model);
    }
    for(Texture2D& texture:ownedTextures_) if(IsTextureValid(texture)) UnloadTexture(texture);
    ownedTextures_.clear();
    parts_.clear();targetBind_.clear();clips_.clear();active_.clear();
    scene_=nullptr;character_.reset();characterPath_.clear();modelValid_=false;
    time_=0;finished_=false;warnedInvalidPose_=false;
}
