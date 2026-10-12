#pragma once
#include "gameplay_lights.h"
#include "enemy_activity.h"
#include "model_loader.h"
#include "scene/scene_definition.h"
#include <limits>
#include <algorithm>
#include <cmath>
namespace gameplay {
// Compact CPU height cache populated once from the actual rendered terrain.
struct MinimapTerrain {
    static constexpr int resolution=128;
    float heights[resolution*resolution]{};
    float x0=0,z0=0,sx=1,sz=1;bool ready=false;
    void Build(const ohos_model::Model& model) {
        ready=false;float x1=-1e30f,z1=-1e30f;x0=z0=1e30f;
        for(const auto& m:model.meshes)for(const auto& v:m.vertices){x0=std::min(x0,v.position[0]);z0=std::min(z0,v.position[2]);x1=std::max(x1,v.position[0]);z1=std::max(z1,v.position[2]);}
        if(x1<=x0 || z1<=z0)return;sx=x1-x0;sz=z1-z0;
        std::fill(heights,heights+resolution*resolution,-1e30f);
        for(const auto& m:model.meshes)for(const auto& v:m.vertices){
            int x=std::min(resolution-1,int((v.position[0]-x0)/sx*resolution));
            int z=std::min(resolution-1,int((v.position[2]-z0)/sz*resolution));
            heights[z*resolution+x]=std::max(heights[z*resolution+x],v.position[1]);
        }ready=true;
    }
    float Height(float x,float z)const {
        int ix=int(std::floor((x-x0)/sx*resolution)),iz=int(std::floor((z-z0)/sz*resolution));
        if(!ready || ix<0 || iz<0 || ix>=resolution || iz>=resolution)return -1e30f;
        return heights[iz*resolution+ix];
    }
};
// North-up local map, 48 metres across; world -Z points to the top.
// Draw via existing overlay primitives, without another scene render pass.
template<class Draw>
inline void DrawMinimap(float w,float h,float px,float pz,float yaw,
    float ex,float ez,bool enemyAlive,const scene::Definition* scene,
    const MinimapTerrain& terrain,Draw draw) {
    const float mn=std::min(w,h), half=mn*.12f, cx=mn*.155f, cy=mn*.17f;
    const float bg[4]={.025f,.055f,.075f,.92f},grid[4]={.55f,.75f,.80f,.22f};
    const float land[4]={.25f,.34f,.29f,.68f},white[4]={1,1,1,1};
    const float red[4]={1,.16f,.13f,1},blue[4]={.18f,.45f,1,1},green[4]={.18f,1,.35f,1};
    draw(cx,cy,half,half,mn*.015f,bg);
    // Bounded 16x16 elevation raster: sea, beaches, vegetation and high ground.
    const float sea=scene && scene->water.enabled?scene->water.height:-2.6f;
    for(int j=0;j<16;j++)for(int i=0;i<16;i++){
        float x=px+(i+.5f-8)*3.f,z=pz+(j+.5f-8)*3.f;
        float height=terrain.Height(x,z),elevation=height-sea;
        float color[4]={.055f,.19f,.27f,.95f};
        if(elevation>0){float t=std::min(1.f,elevation/8);
            color[0]=.20f+.24f*t;color[1]=.36f+.10f*t;color[2]=.18f+.22f*t;
            if(elevation<.45f){color[0]=.65f;color[1]=.60f;color[2]=.38f;}}
        draw(cx+(i+.5f-8)*half/8,cy+(j+.5f-8)*half/8,half/16,half/16,0,color);
    }
    // The default platform is a 40 x 40 metre square centred at the origin.
    float x0=std::max(-half,(-20-px)*half/24),x1=std::min(half,(20-px)*half/24);
    float z0=std::max(-half,(-20-pz)*half/24),z1=std::min(half,(20-pz)*half/24);
    if(x1>x0 && z1>z0)draw(cx+(x0+x1)*.5f,cy+(z0+z1)*.5f,(x1-x0)*.5f,(z1-z0)*.5f,0,land);
    for(int i=-1;i<=1;i++){draw(cx+i*half*.5f,cy,mn*.0007f,half,0,grid);draw(cx,cy+i*half*.5f,half,mn*.0007f,0,grid);}
    // Static prop footprints follow authored position, scale and rotation.
    if(scene)for(const auto& entity:scene->entities){
        if(!entity.visible || !entity.importAsStaticCube || entity.groundSurface)continue;
        auto t=scene::RuntimeTransform(*scene,entity,-.94f);float matrix[16];scene::BuildModelMatrix(t,matrix);
        float hx=std::abs(matrix[0])+std::abs(matrix[4])+std::abs(matrix[8]);
        float hz=std::abs(matrix[2])+std::abs(matrix[6])+std::abs(matrix[10]);
        float left=std::max(-24.f,t.position.x-hx-px),right=std::min(24.f,t.position.x+hx-px);
        float top=std::max(-24.f,t.position.z-hz-pz),bottom=std::min(24.f,t.position.z+hz-pz);
        const float wall[4]={.72f,.76f,.79f,.95f};
        if(right>left && bottom>top)draw(cx+(left+right)*half/48,cy+(top+bottom)*half/48,
            (right-left)*half/48,(bottom-top)*half/48,mn*.002f,wall);
    }
    // Outline the healing area and enemy activity zone with clipped dots.
    auto ring=[&](float x,float z,float radius,const float* color){
        for(int i=0;i<32;i++){float angle=i*6.2831853f/32;
            float dx=x+std::cos(angle)*radius-px,dz=z+std::sin(angle)*radius-pz;
            if(std::abs(dx)<23.5f && std::abs(dz)<23.5f)draw(cx+dx*half/24,cy+dz*half/24,mn*.0017f,mn*.0017f,mn*.0017f,color);
        }
    };
    ring(kHealingLightX,kHealingLightZ,kHealingRadius,green);
    ring(kEnemySpawnX,kEnemySpawnZ,kEnemyActivityRadius,red);
    auto mark=[&](float x,float z,float radius,const float* color){
        float dx=(x-px)*half/24,dz=(z-pz)*half/24;
        float limit=half-radius-mn*.003f;
        float scale=std::max(1.f,std::max(std::abs(dx),std::abs(dz))/limit);
        draw(cx+dx/scale,cy+dz/scale,radius,radius,radius,color);
    };
    mark(kPlayerSpawnX,kPlayerSpawnZ,mn*.006f,blue);
    mark(kEnemySpawnX,kEnemySpawnZ,mn*.006f,red);
    mark(kHealingLightX,kHealingLightZ,mn*.007f,green);
    if(enemyAlive)mark(ex,ez,mn*.009f,red);
    // Bright north tick and frame keep orientation visible over the terrain.
    draw(cx,cy-half+mn*.003f,mn*.016f,mn*.002f,0,white);
    for(int side=-1;side<=1;side+=2){
        draw(cx+side*half,cy,mn*.0008f,half,0,grid);
        draw(cx,cy+side*half,half,mn*.0008f,0,grid);
    }
    draw(cx,cy,mn*.008f,mn*.008f,mn*.008f,white);
    draw(cx+std::sin(yaw)*mn*.017f,cy+std::cos(yaw)*mn*.017f,mn*.0035f,mn*.0035f,mn*.0035f,white);
}
} // namespace gameplay
