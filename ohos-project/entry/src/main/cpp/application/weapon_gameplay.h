#pragma once
#include "model_loader.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace gameplay {
struct WeaponButton { float x,y,r; };
inline WeaponButton PistolButton(int index,float w,float h) {
    return {w*0.79f,h*(0.23f+0.16f*index),std::min(w,h)*0.052f};
}
inline int HitPistolButton(float x,float y,float w,float h,bool equipped) {
    for(int i=0;i<(equipped?3:1);++i) {
        auto b=PistolButton(i,w,h);float dx=x-b.x,dy=y-b.y;
        if(dx*dx+dy*dy<=b.r*b.r)return i;
    }
    return -1;
}
struct PistolState {
    bool equipped=false, aiming=false, pendingFire=false, fired=false, hit=false;
    float cooldown=0, shotAge=10, hitFlash=0;
    void Reset(){*this=PistolState{};}
    void Button(int i) {
        if(i==0){equipped=!equipped;aiming=false;pendingFire=false;shotAge=10;}
        else if(equipped && i==1)aiming=!aiming;
        else if(equipped && i==2)pendingFire=true;
    }
    void Tick(float dt,bool allowed) {
        cooldown=std::max(0.f,cooldown-dt);shotAge+=dt;
        hitFlash=std::max(0.f,hitFlash-dt);fired=false;hit=false;
        if(pendingFire && equipped && allowed && cooldown<=0){
            fired=true;cooldown=.35f;shotAge=0;
        }
        pendingFire=false;
    }
};
inline const ohos_model::AnimationClip* PistolClip(
    const std::vector<ohos_model::AnimationClip>& clips,const PistolState& gun,float pitch) {
    const char* name=gun.shotAge<.30f?"Pistol_Shoot":
        (!gun.aiming?"Pistol_Idle_Loop":"Pistol_Aim_Neutral");
    for(auto& clip:clips)if(clip.name==name)return &clip;
    return nullptr;
}
// Blend matching TRS channels continuously; quaternion hemisphere correction
// avoids a long rotation when source clips encode opposite quaternion signs.
template<class Sampler>
inline void BlendPistolAim(const std::vector<ohos_model::AnimationClip>& clips,
    const PistolState& gun,float pitch,const ohos_model::AnimationChannel& channel,
    Sampler sample,float* value) {
    if(!gun.aiming || gun.shotAge<.30f)return;
    float weight=std::min(1.f,std::abs(pitch)/1.0f);
    const char* name=pitch>=0?"Pistol_Aim_Down":"Pistol_Aim_Up";
    for(const auto& clip:clips)if(clip.name==name){
        for(const auto& other:clip.channels)if(other.nodeIndex==channel.nodeIndex && other.path==channel.path){
            float endpoint[4]{};sample(other,0.f,endpoint);
            int count=channel.path==1?4:3;
            if(count==4){float dot=0;for(int i=0;i<4;i++)dot+=value[i]*endpoint[i];
                if(dot<0)for(float& v:endpoint)v=-v;}
            for(int i=0;i<count;i++)value[i]+=(endpoint[i]-value[i])*weight;
            if(count==4){float length=0;for(int i=0;i<4;i++)length+=value[i]*value[i];
                if(length>1e-12f){length=std::sqrt(length);for(int i=0;i<4;i++)value[i]/=length;}}
            return;
        }
        return;
    }
}
inline bool WeaponUpperBody(const std::vector<ohos_model::Node>& nodes,size_t index) {
    for(size_t steps=0;index<nodes.size() && steps<nodes.size();++steps){
        if(nodes[index].name=="DEF-spine.001")return true;
        const auto parent=nodes[index].parent;if(parent<0)break;index=static_cast<size_t>(parent);
    }
    return false;
}
inline void WeaponMultiply(const float* a,const float* b,float* out) {
    float t[16]{};for(int c=0;c<4;++c)for(int row=0;row<4;++row)
        for(int k=0;k<4;++k)t[c*4+row]+=a[k*4+row]*b[c*4+k];
    std::memcpy(out,t,sizeof(t));
}
inline void PistolMount(const float* player,const float* normalise,const float* hand,float* out) {
    float pose[16],world[16];WeaponMultiply(normalise,hand,pose);
    // Preserve the hand position, removing inherited rig scale from the grip.
    for(int c=0;c<3;++c){float n=std::sqrt(pose[c*4]*pose[c*4]+pose[c*4+1]*pose[c*4+1]+pose[c*4+2]*pose[c*4+2]);
        if(n>1e-6f)for(int row=0;row<3;++row)pose[c*4+row]/=n;}
    // Calibrated from DEF-hand.R in Pistol_Aim_Neutral. +X barrel -> +Z forward.
    const float mount[16]={0.029999288f,0.982385913f,-0.184439476f,0.000000000f,-0.041918582f,0.185596763f,0.981731483f,0.000000000f,0.998670554f,-0.021720015f,0.046747986f,0.000000000f,0.000813600f,0.096766587f,0.027578364f,1.000000000f};
    WeaponMultiply(pose,mount,world);
    for(int c=0;c<3;++c)for(int row=0;row<3;++row)world[c*4+row]*=.18f;
    WeaponMultiply(player,world,out);
}
// Ray versus the actual vertical character capsule, returns nearest positive t.
inline float RaySphere(float ox,float oy,float oz,float dx,float dy,float dz,
    float cx,float cy,float cz,float radius,float limit) {
    float x=ox-cx,y=oy-cy,z=oz-cz,b=x*dx+y*dy+z*dz,c=x*x+y*y+z*z-radius*radius;
    float disc=b*b-c;if(disc<0)return limit;
    float t=-b-std::sqrt(disc);if(t<0)t=-b+std::sqrt(disc);
    return t>=0 && t<limit?t:limit;
}
inline float RayEnemy(float ox,float oy,float oz,float dx,float dy,float dz,
    float x,float visualY,float z,float limit=60.f) {
    const float radius=.35f, lo=visualY-.96f+radius, hi=visualY+.92f-radius;
    float best=RaySphere(ox,oy,oz,dx,dy,dz,x,lo,z,radius,limit);
    best=RaySphere(ox,oy,oz,dx,dy,dz,x,hi,z,radius,best);
    float a=dx*dx+dz*dz,b=(ox-x)*dx+(oz-z)*dz,c=(ox-x)*(ox-x)+(oz-z)*(oz-z)-radius*radius;
    float disc=b*b-a*c;
    if(a>1e-8f && disc>=0){for(int i=0;i<2;++i){float t=(-b+(i?1.f:-1.f)*std::sqrt(disc))/a;
        float y=oy+t*dy;if(t>=0 && t<best && y>=lo && y<=hi)best=t;}}
    return best;
}
} // namespace gameplay
