#pragma once
#include "inventory.h"
#include <algorithm>
#include <cstdio>
namespace inventory_ui {
struct Button {float cx,cy,hw,hh,r;};
inline Button Layout(int index,float w,float h){float mn=std::min(w,h),width=std::min(w*.88f,mn*1.65f);
 if(index==0)return {w*.5f+width*.45f,h*.145f,mn*.025f,mn*.025f,mn*.01f};
 if(index>=1&&index<=3)return {w*.5f+(index-2)*width*.29f,h*.29f,width*.125f,h*.052f,mn*.012f};
 int i=index-10;return {w*.5f+(i%5-2)*width*.18f,h*(.46f+.11f*(i/5)),width*.08f,h*.044f,mn*.010f};
}
template<class Rect,class Text>
void Draw(float w,float h,const inventory::State& state,Rect rect,Text text){
 float mn=std::min(w,h),width=std::min(w*.88f,mn*1.65f);
 const float bg[4]={.02f,.035f,.055f,.6f},sheet[4]={.08f,.13f,.19f,.98f};
 const float slot[4]={.12f,.22f,.29f,.95f},equipped[4]={.12f,.37f,.34f,.98f},white[4]={1,1,1,1};
 rect(w*.5f,h*.5f,w*.5f,h*.5f,0,bg);rect(w*.5f,h*.51f,width*.5f,h*.42f,mn*.02f,sheet);
 text("背包",w*.5f,h*.145f,mn*.043f,white);
 auto close=Layout(0,w,h);rect(close.cx,close.cy,close.hw,close.hh,close.r,slot);text("X",close.cx,close.cy,mn*.024f,white);
 const char* names[]={"武器","头盔","护甲"};
 for(int i=0;i<3;i++){auto b=Layout(i+1,w,h);auto d=state.EquippedDef(i);
  text(names[i],b.cx,h*.215f,mn*.025f,white);rect(b.cx,b.cy,b.hw,b.hh,b.r,d?equipped:slot);
  text(d?d->name:"空",b.cx,b.cy-h*.015f,mn*.027f,white);
  char desc[64];if(d)std::snprintf(desc,sizeof(desc),"攻击×%.2f 防御×%.2f",d->attack,d->defense);else std::snprintf(desc,sizeof(desc),"未装备");
  text(desc,b.cx,b.cy+h*.022f,mn*.016f,white);
 }
 text("轻点物品装备 轻点装备栏卸下",w*.5f,h*.375f,mn*.023f,white);
 for(int i=0;i<inventory::kBagCapacity;i++){
  auto b=Layout(i+10,w,h);auto d=state.DefOf(i);rect(b.cx,b.cy,b.hw,b.hh,b.r,slot);
  if(!d)continue;float color[4]={d->color[0],d->color[1],d->color[2],1};
  rect(b.cx,b.cy-b.hh*.65f,b.hw*.75f,mn*.002f,0,color);
  char label[32];if(d->maxCount>1)std::snprintf(label,sizeof(label),"%s ×%d",d->name,state.bag[i].count);else std::snprintf(label,sizeof(label),"%s",d->name);
  text(label,b.cx,b.cy,mn*.026f,white);
 }
 auto b=state.Stats();char stats[128];
 std::snprintf(stats,sizeof(stats),"攻击×%.2f    防御×%.2f    移速×%.2f",b.attack,b.defense,b.speed);
 text(stats,w*.5f,h*.80f,mn*.030f,white);
 text("药水已绑定快捷栏 恢复30血量",w*.5f,h*.865f,mn*.022f,white);
}
}
