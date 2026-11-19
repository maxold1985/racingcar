#include <assimp/matrix4x4.h>
#include <assimp/vector3.h>
#include <assimp/vector3.inl>
#include <assimp/matrix4x4.inl>
#include <cmath>
#include <cstdio>

static aiMatrix4x4 translation(float x,float y,float z) {
    aiMatrix4x4 m;
    m.a4=x;m.b4=y;m.c4=z;
    return m;
}
static bool closeEnough(const aiVector3D& a,const aiVector3D& b) {
    const auto d=a-b;
    return d.SquareLength()<1e-6f;
}
static aiMatrix4x4 retarget(const aiMatrix4x4& targetBind,
                           const aiMatrix4x4& sourceBind,
                           const aiMatrix4x4& sourcePose) {
    aiMatrix4x4 inverseSource=sourceBind;
    aiMatrix4x4 inverseTarget=targetBind;
    inverseSource.Inverse();
    inverseTarget.Inverse();
    return targetBind*inverseSource*sourcePose*inverseTarget;
}
int main() {
    // Rig and clip use different world origins. The rest pose MUST be preserved.
    const aiMatrix4x4 targetBind=translation(-3.0f,8.0f,2.0f);
    const aiMatrix4x4 sourceBind=translation(15.0f,4.0f,-8.0f);
    const aiVector3D vertex(-3.0f,9.0f,3.0f);
    const aiVector3D atRest=retarget(targetBind,sourceBind,sourceBind)*vertex;
    if(!closeEnough(atRest,vertex)) { std::fputs("FAIL: bind pose changed\n",stderr); return 1; }

    // A 90-degree source bone rotation must happen about the TARGET joint.
    aiMatrix4x4 rotation;
    rotation.a1=0.0f;rotation.a3=1.0f;
    rotation.c1=-1.0f;rotation.c3=0.0f;
    const aiMatrix4x4 sourceFrame=sourceBind*rotation;
    const aiMatrix4x4 transformed=retarget(targetBind,sourceBind,sourceFrame);
    const aiMatrix4x4 expected=targetBind*rotation*translation(3.0f,-8.0f,-2.0f);
    if(!closeEnough(transformed*vertex,expected*vertex)) { std::fputs("FAIL: joint retarget\n",stderr); return 2; }

    // Weighted linear blend skinning at bind pose may never displace vertices.
    const aiMatrix4x4 bindA=translation(1,0,0);
    const aiMatrix4x4 bindB=translation(0,1,0);
    const aiVector3D p(3,4,5);
    const aiVector3D blended=
        (retarget(bindA,sourceBind,sourceBind)*p)*0.3f+
        (retarget(bindB,sourceBind,sourceBind)*p)*0.7f;
    if(!closeEnough(blended,p)) { std::fputs("FAIL: weighted bind\n",stderr); return 3; }
    // Real Mixamo clips begin in an animated running pose, not T-pose.
    // A mesh node may itself be translated: its inverse must be removed
    // once when skinning vertices already transformed to model space.
    const aiMatrix4x4 meshBind=translation(3,0,0);
    const aiMatrix4x4 boneBind=translation(3,4,0);
    const aiMatrix4x4 meshLocalOffset=translation(0,-4,0);
    aiMatrix4x4 meshInverse=meshBind;
    meshInverse.Inverse();
    const aiMatrix4x4 offsetModel=meshLocalOffset*meshInverse;
    const aiVector3D modelVertex(3,5,0);
    if(!closeEnough((boneBind*offsetModel)*modelVertex,modelVertex)) {
        std::fputs("FAIL: mesh-node bind compensation\\n",stderr);
        return 4;
    }
    aiMatrix4x4 firstFrameRotation;
    firstFrameRotation.a1=0.0f;firstFrameRotation.a2=-1.0f;
    firstFrameRotation.b1=1.0f;firstFrameRotation.b2=0.0f;
    const aiMatrix4x4 firstFrameGlobal=boneBind*firstFrameRotation;
    const aiVector3D firstFrame=(firstFrameGlobal*offsetModel)*modelVertex;
    if(!closeEnough(firstFrame,aiVector3D(2,4,0)) ||
       closeEnough(firstFrame,modelVertex)) {
        std::fputs("FAIL: running first frame was incorrectly reset to bind pose\\n",stderr);
        return 5;
    }
    std::puts("Mixamo bind matrices and animated first frame OK");
    return 0;
}
