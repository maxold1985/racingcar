#include "rv_animation.hpp"
#include "raymath.h"
#include <assimp/postprocess.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
constexpr unsigned int importFlags =
    aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
    aiProcess_LimitBoneWeights | aiProcess_GenSmoothNormals;
std::string boneKey(std::string name) {
    // Mixamo exports may use "mixamorig:Hips" or "Armature|mixamorig:Hips".
    const auto separator=name.find_last_of(":|");
    if(separator!=std::string::npos) name=name.substr(separator+1);
    return name;
}
void findMeshNodes(const aiNode* node,const aiMatrix4x4& parent,
                   std::unordered_map<unsigned,aiMatrix4x4>& transforms) {
    const aiMatrix4x4 global=parent*node->mTransformation;
    for(unsigned i=0;i<node->mNumMeshes;++i) transforms[node->mMeshes[i]]=global;
    for(unsigned i=0;i<node->mNumChildren;++i)
        findMeshNodes(node->mChildren[i],global,transforms);
}
aiVector3D interpolatePosition(const aiNodeAnim* c,double t,const aiVector3D& fallback) {
    if (!c || !c->mNumPositionKeys) return fallback;
    if (c->mNumPositionKeys==1 || t<=c->mPositionKeys[0].mTime) return c->mPositionKeys[0].mValue;
    for(unsigned i=0;i+1<c->mNumPositionKeys;++i) {
        const auto& a=c->mPositionKeys[i];const auto& b=c->mPositionKeys[i+1];
        if(t<=b.mTime) {
            const float alpha=float((t-a.mTime)/std::max(1e-9,b.mTime-a.mTime));
            return a.mValue*(1-alpha)+b.mValue*alpha;
        }
    }
    return c->mPositionKeys[c->mNumPositionKeys-1].mValue;
}
aiQuaternion interpolateRotation(const aiNodeAnim* c,double t,const aiQuaternion& fallback) {
    if(!c || !c->mNumRotationKeys) return fallback;
    if(c->mNumRotationKeys==1 || t<=c->mRotationKeys[0].mTime) return c->mRotationKeys[0].mValue;
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
aiVector3D interpolateScale(const aiNodeAnim* c,double t,const aiVector3D& fallback) {
    if(!c || !c->mNumScalingKeys) return fallback;
    if(c->mNumScalingKeys==1 || t<=c->mScalingKeys[0].mTime) return c->mScalingKeys[0].mValue;
    for(unsigned i=0;i+1<c->mNumScalingKeys;++i) {
        const auto& a=c->mScalingKeys[i];const auto& b=c->mScalingKeys[i+1];
        if(t<=b.mTime) {
            const float alpha=float((t-a.mTime)/std::max(1e-9,b.mTime-a.mTime));
            return a.mValue*(1-alpha)+b.mValue*alpha;
        }
    }
    return c->mScalingKeys[c->mNumScalingKeys-1].mValue;
}
}

bool RVAnimation::loadCharacter(const char* path) {
    unload();
    if(!path || !path[0]) return false;
    character_=std::make_unique<Assimp::Importer>();
    scene_=character_->ReadFile(path,importFlags);
    if(!scene_ || !scene_->mRootNode || !scene_->HasMeshes()) {
        std::fprintf(stderr,"Assimp character import failed: %s (%s)\n",
                     path,character_->GetErrorString());
        unload();
        return false;
    }
    const float extreme=std::numeric_limits<float>::max();
    aiVector3D low(extreme,extreme,extreme);
    aiVector3D high(-extreme,-extreme,-extreme);
    std::unordered_map<unsigned,aiMatrix4x4> meshNodes;
    findMeshNodes(scene_->mRootNode,aiMatrix4x4(),meshNodes);
    for(unsigned mi=0;mi<scene_->mNumMeshes;++mi) {
        const aiMesh* src=scene_->mMeshes[mi];
        if(!src->mNumVertices || !src->mNumFaces) continue;
        Part part;
        part.bindVertices.assign(src->mVertices,src->mVertices+src->mNumVertices);
        // Meshes are in their node-local bind space; preserve that node transform.
        const auto nodeIt=meshNodes.find(mi);
        const aiMatrix4x4 meshBind=nodeIt==meshNodes.end()?aiMatrix4x4():nodeIt->second;
        Mesh mesh{};
        mesh.vertexCount=int(src->mNumVertices);
        mesh.triangleCount=int(src->mNumFaces);
        mesh.vertices=(float*)MemAlloc(size_t(mesh.vertexCount)*3*sizeof(float));
        mesh.normals=(float*)MemAlloc(size_t(mesh.vertexCount)*3*sizeof(float));
        mesh.texcoords=(float*)MemAlloc(size_t(mesh.vertexCount)*2*sizeof(float));
        mesh.indices=(unsigned short*)MemAlloc(size_t(mesh.triangleCount)*3*sizeof(unsigned short));
        if(src->mNumVertices>65535) {
            std::fprintf(stderr,"FBX mesh exceeds raylib 16-bit index limit\n");
            MemFree(mesh.vertices);MemFree(mesh.normals);MemFree(mesh.texcoords);MemFree(mesh.indices);
            continue;
        }
        for(unsigned i=0;i<src->mNumVertices;++i) {
            const auto v=meshBind*src->mVertices[i];
            low.x=std::min(low.x,v.x);low.y=std::min(low.y,v.y);low.z=std::min(low.z,v.z);
            high.x=std::max(high.x,v.x);high.y=std::max(high.y,v.y);high.z=std::max(high.z,v.z);
            mesh.vertices[i*3]=v.x;mesh.vertices[i*3+1]=v.y;mesh.vertices[i*3+2]=v.z;
            const auto n=src->HasNormals()?src->mNormals[i]:aiVector3D(0,1,0);
            mesh.normals[i*3]=n.x;mesh.normals[i*3+1]=n.y;mesh.normals[i*3+2]=n.z;
            const auto uv=src->HasTextureCoords(0)?src->mTextureCoords[0][i]:aiVector3D();
            mesh.texcoords[i*2]=uv.x;mesh.texcoords[i*2+1]=1.0f-uv.y;
        }
        for(unsigned i=0;i<src->mNumFaces;++i) {
            const aiFace& face=src->mFaces[i];
            if(face.mNumIndices!=3) { mesh.indices[i*3]=mesh.indices[i*3+1]=mesh.indices[i*3+2]=0;continue; }
            for(int j=0;j<3;++j) mesh.indices[i*3+j]=(unsigned short)face.mIndices[j];
        }
        for(unsigned b=0;b<src->mNumBones;++b) {
            const aiBone* sb=src->mBones[b];
            Bone bone;
            bone.name=boneKey(sb->mName.C_Str());
            // Assimp offset already maps mesh-local bind vertices into bone space.
            bone.offset=sb->mOffsetMatrix;
            for(unsigned w=0;w<sb->mNumWeights;++w)
                bone.weights.push_back({sb->mWeights[w].mVertexId,sb->mWeights[w].mWeight});
            part.bones.push_back(std::move(bone));
        }
        UploadMesh(&mesh,true);
        part.model=LoadModelFromMesh(mesh);
        parts_.push_back(std::move(part));
    }
    if(parts_.empty()) { unload();return false; }
    center_=aiVector3D((low.x+high.x)*0.5f,low.y,(low.z+high.z)*0.5f);
    const float h=high.y-low.y;
    scale_=(h>0.001f)?1.75f/h:1.0f;
    modelValid_=true;
    if(scene_->HasAnimations()) {
        Clip embedded;
        embedded.animation=scene_->mAnimations[0];
        embedded.duration=embedded.animation->mDuration;
        embedded.ticksPerSecond=embedded.animation->mTicksPerSecond>0?
            embedded.animation->mTicksPerSecond:25.0;
        clips_["Default"]=std::move(embedded);
    }
    std::fprintf(stderr,"Animated character: %u meshes, %zu parts, scale %.5f\n",
                 scene_->mNumMeshes,parts_.size(),scale_);
    return true;
}
bool RVAnimation::loadClip(const std::string& name,const char* path) {
    if(!modelValid_ || !path || !path[0]) return false;
    Clip clip;
    clip.importer=std::make_unique<Assimp::Importer>();
    const aiScene* imported=clip.importer->ReadFile(path,importFlags);
    if(!imported || !imported->HasAnimations()) {
        std::fprintf(stderr,"FBX animation import failed: %s (%s)\n",
                     path,clip.importer->GetErrorString());
        return false;
    }
    clip.animation=imported->mAnimations[0];
    clip.duration=clip.animation->mDuration;
    clip.ticksPerSecond=clip.animation->mTicksPerSecond>0?clip.animation->mTicksPerSecond:25.0;
    clips_[name]=std::move(clip);
    std::fprintf(stderr,"Animation %s loaded: %s\n",name.c_str(),path);
    return true;
}
bool RVAnimation::play(const std::string& name,bool loop) {
    if(!modelValid_ || clips_.find(name)==clips_.end()) return false;
    if(active_==name && loop_==loop && !(finished_&&!loop)) return true;
    active_=name;loop_=loop;time_=0.0;finished_=false;
    return true;
}
void RVAnimation::evaluate(const aiNode* node,const aiMatrix4x4& parent,
    const aiAnimation* animation,double tick,
    std::unordered_map<std::string,aiMatrix4x4>& transforms) const {
    aiMatrix4x4 local=node->mTransformation;
    if(animation) {
        for(unsigned i=0;i<animation->mNumChannels;++i) {
            const aiNodeAnim* c=animation->mChannels[i];
            if(boneKey(node->mName.C_Str())!=boneKey(c->mNodeName.C_Str())) continue;
            aiVector3D baseScale,basePosition;aiQuaternion baseRotation;
            node->mTransformation.Decompose(baseScale,baseRotation,basePosition);
            const aiVector3D p=interpolatePosition(c,tick,basePosition);
            const aiQuaternion r=interpolateRotation(c,tick,baseRotation);
            const aiVector3D sc=interpolateScale(c,tick,baseScale);
            local=aiMatrix4x4(sc.x,0,0,0, 0,sc.y,0,0, 0,0,sc.z,0, 0,0,0,1);
            local=aiMatrix4x4(r.GetMatrix())*local;
            local.a4=p.x;local.b4=p.y;local.c4=p.z;
            break;
        }
    }
    const aiMatrix4x4 global=parent*local;
    transforms[boneKey(node->mName.C_Str())]=global;
    for(unsigned i=0;i<node->mNumChildren;++i)
        evaluate(node->mChildren[i],global,animation,tick,transforms);
}
void RVAnimation::update(float dt) {
    if(!modelValid_ || active_.empty()) return;
    auto it=clips_.find(active_);
    if(it==clips_.end()) return;
    const Clip& clip=it->second;
    time_+=std::max(0.0f,dt)*clip.ticksPerSecond;
    double tick=time_;
    if(clip.duration>0) {
        if(loop_) tick=std::fmod(tick,clip.duration);
        else if(tick>=clip.duration) { tick=clip.duration;finished_=true; }
    }
    std::unordered_map<std::string,aiMatrix4x4> transforms;
    evaluate(scene_->mRootNode,aiMatrix4x4(),clip.animation,tick,transforms);
    // Transform from scene-global space back into the model root space.
    // Assimp bone offsets already encode the mesh bind pose.
    aiMatrix4x4 rootInverse=scene_->mRootNode->mTransformation;
    rootInverse.Inverse();
    for(Part& part:parts_) {
        if(part.bones.empty()) continue;
        Mesh& mesh=part.model.meshes[0];
        std::vector<aiVector3D> result(part.bindVertices.size(),aiVector3D());
        std::vector<float> total(part.bindVertices.size(),0.0f);
        for(const Bone& bone:part.bones) {
            const auto found=transforms.find(bone.name);
            if(found==transforms.end()) continue;
            const aiMatrix4x4 transform=rootInverse*found->second*bone.offset;
            for(const Weight& w:bone.weights) {
                if(w.vertex>=result.size()) continue;
                result[w.vertex]+=(transform*part.bindVertices[w.vertex])*w.weight;
                total[w.vertex]+=w.weight;
            }
        }
        for(size_t i=0;i<result.size();++i) {
            const aiVector3D v=total[i]>0.0001f?
                result[i]/total[i]:part.bindVertices[i];
            mesh.vertices[i*3]=v.x;mesh.vertices[i*3+1]=v.y;mesh.vertices[i*3+2]=v.z;
        }
        UpdateMeshBuffer(mesh,0,mesh.vertices,int(result.size()*3*sizeof(float)),0);
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
    for(Part& part:parts_) UnloadModel(part.model);
    parts_.clear();clips_.clear();active_.clear();scene_=nullptr;
    character_.reset();modelValid_=false;time_=0;finished_=false;
}
