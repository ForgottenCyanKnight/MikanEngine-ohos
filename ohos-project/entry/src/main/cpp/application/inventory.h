#pragma once
#include <array>
#include <algorithm>
namespace inventory {
enum class SlotType {Weapon=0,Helmet,Armor,Count};
struct ItemDef {const char* name;SlotType slot;float color[3];int maxCount;float attack=1,defense=1,speed=1;};
inline constexpr ItemDef kItemDefs[]={
 {"手枪",SlotType::Weapon,{.70f,.75f,.82f},1,1.30f,1,1},
 {"铁盔",SlotType::Helmet,{.60f,.64f,.70f},1,1,1.15f,1},
 {"皮甲",SlotType::Armor,{.55f,.38f,.22f},1,1,1.25f,1.05f},
 {"法袍",SlotType::Armor,{.35f,.45f,.80f},1,1.10f,1.10f,1.10f},
 {"药水",SlotType::Count,{.85f,.30f,.35f},3,1,1,1}};
inline constexpr int kItemDefCount=5,kSlotCount=3,kBagCapacity=15,kNoItem=-1;
struct BagEntry {int defId=-1,count=0;};
struct Bonuses {float attack=1,defense=1,speed=1;};
struct State {
 std::array<BagEntry,kBagCapacity> bag{};
 // Equipment owns a definition ID, never an index into an emptied bag cell.
 std::array<int,kSlotCount> equipped{};
 State(){equipped.fill(kNoItem);for(int i=0;i<kItemDefCount;i++)bag[i]={i,i==4?3:1};}
 const ItemDef* DefOf(int cell)const {
  if(cell<0||cell>=kBagCapacity||bag[cell].count<=0)return nullptr;
  int id=bag[cell].defId;return id>=0&&id<kItemDefCount?&kItemDefs[id]:nullptr;
 }
 const ItemDef* EquippedDef(int slot)const {
  if(slot<0||slot>=kSlotCount)return nullptr;int id=equipped[slot];return id>=0&&id<kItemDefCount?&kItemDefs[id]:nullptr;
 }
 int TargetSlot(int cell)const {auto d=DefOf(cell);return !d||d->slot==SlotType::Count?kNoItem:static_cast<int>(d->slot);}
 bool TapBag(int cell){
  auto d=DefOf(cell);if(!d)return false;
  if(d->slot==SlotType::Count){if(--bag[cell].count==0)bag[cell]={};return true;}
  int slot=static_cast<int>(d->slot),old=equipped[slot];equipped[slot]=bag[cell].defId;
  bag[cell]=old==kNoItem?BagEntry{}:BagEntry{old,1};return true;
 }
 bool TapEquip(int slot){
  if(!EquippedDef(slot))return false;
  for(auto& cell:bag)if(cell.defId==kNoItem){cell={equipped[slot],1};equipped[slot]=kNoItem;return true;}return false;
 }
 bool HasPistol()const{return equipped[0]==0;}
 int PotionCount()const{int count=0;for(int i=0;i<kBagCapacity;i++)if(auto d=DefOf(i))if(d->slot==SlotType::Count)count+=bag[i].count;return count;}
 bool UsePotion(float& health,float maximum){
  if(health<=0 || health>=maximum)return false;
  for(int i=0;i<kBagCapacity;i++)if(auto d=DefOf(i))if(d->slot==SlotType::Count){
   if(!TapBag(i))return false;health=std::min(maximum,health+30.f);return true;
  }return false;
 }
 Bonuses Stats()const {Bonuses b;for(int i=0;i<kSlotCount;i++)if(auto d=EquippedDef(i)){b.attack*=d->attack;b.defense*=d->defense;b.speed*=d->speed;}return b;}
};
}
