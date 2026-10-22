#pragma once
#include "inventory.h"
#include <algorithm>
#include <cstdio>
namespace shop_ui {
struct Button {float cx,cy,hw,hh,r;};
// Shop catalog: gold-priced consumables and equipment.  Equipment lands in
// the bag (equip it from the inventory sheet); ammo tops up the pistol
// reserve; potions stack onto the existing potion cell.
enum Item {kPotion=0,kAmmo,kHelmet,kArmor,kRobe,kItemCount};
struct Entry {const char* name;int price;const char* desc;};
inline const Entry& Def(int index){
 static const Entry kEntries[kItemCount]={
  {"药水",50,"恢复30血量 入背包"},
  {"弹药",40,"手枪备弹 +12发"},
  {"铁盔",120,"防御×1.15 入背包"},
  {"皮甲",150,"防御×1.25 移速×1.05"},
  {"法袍",200,"攻防移速 ×1.10"}};
 return kEntries[index];
}
// index 0 = close button; index 10+i = buy button of row i.
inline Button Layout(int index,float w,float h){
 float mn=std::min(w,h),width=std::min(w*.88f,mn*1.65f);
 if(index==0)return {w*.5f+width*.45f,h*.145f,mn*.040f,mn*.040f,mn*.016f};
 int i=index-10;
 return {w*.5f+width*.28f,h*(.275f+.105f*i),width*.14f,h*.048f,mn*.012f};
}
template<class Rect,class Text>
void Draw(float w,float h,int gold,Rect rect,Text text){
 float mn=std::min(w,h),width=std::min(w*.88f,mn*1.65f);
 const float bg[4]={.02f,.035f,.055f,.6f},sheet[4]={.08f,.13f,.19f,.98f};
 const float slot[4]={.12f,.22f,.29f,.95f},white[4]={1,1,1,1};
 const float goldColor[4]={.85f,.68f,.20f,.95f};
 const float canBuy[4]={.12f,.37f,.34f,.98f},noGold[4]={.20f,.22f,.25f,.90f};
 rect(w*.5f,h*.5f,w*.5f,h*.5f,0,bg);rect(w*.5f,h*.51f,width*.5f,h*.42f,mn*.02f,sheet);
 text("商店",w*.5f,h*.145f,mn*.043f,white);
 auto close=Layout(0,w,h);rect(close.cx,close.cy,close.hw,close.hh,close.r,slot);text("X",close.cx,close.cy,mn*.038f,white);
 char purse[32];std::snprintf(purse,sizeof(purse),"金币 %d",gold);
 text(purse,w*.5f-width*.30f,h*.145f,mn*.030f,goldColor);
 for(int i=0;i<kItemCount;i++){
  auto b=Layout(i+10,w,h);const Entry& e=Def(i);
  const float y=b.cy;
  text(e.name,w*.5f-width*.28f,y-mn*.016f,mn*.028f,white);
  text(e.desc,w*.5f-width*.28f,y+mn*.018f,mn*.016f,white);
  rect(b.cx,b.cy,b.hw,b.hh,b.r,gold>=e.price?canBuy:noGold);
  char price[32];std::snprintf(price,sizeof(price),"购买 %d",e.price);
  text(price,b.cx,b.cy,mn*.024f,white);
 }
 text("击杀敌人获得金币 装备请到背包中穿戴",w*.5f,h*.80f,mn*.022f,white);
}
}
