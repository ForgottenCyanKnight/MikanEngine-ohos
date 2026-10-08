#pragma once

// Shared inventory state for the OHOS demo (GLES + Vulkan backends render
// and hit-test their own overlays, but both consume this one state machine).
//
// Model: every bag entry is an *instance* of an ItemDef.  Equip/unequip
// simply moves an instance between the bag grid and the per-slot equipment
// array; potions are consumables (no slot) that decrement on use.

#include <array>
#include <vector>

namespace inventory {

enum class SlotType { Weapon = 0, Helmet, Armor, Count };

struct ItemDef {
    const char* name;      // SDF atlas must contain these glyphs
    SlotType slot;         // SlotType::Count -> consumable, cannot equip
    float color[3];        // icon tint
    int maxCount;          // stack size for consumables
};

inline constexpr ItemDef kItemDefs[] = {
    {"铁剑", SlotType::Weapon, {0.85f, 0.87f, 0.92f}, 1},
    {"长弓", SlotType::Weapon, {0.72f, 0.55f, 0.30f}, 1},
    {"铁盔", SlotType::Helmet, {0.60f, 0.64f, 0.70f}, 1},
    {"皮甲", SlotType::Armor, {0.55f, 0.38f, 0.22f}, 1},
    {"法袍", SlotType::Armor, {0.35f, 0.45f, 0.80f}, 1},
    {"药水", SlotType::Count, {0.85f, 0.30f, 0.35f}, 3},
};
inline constexpr int kItemDefCount = static_cast<int>(sizeof(kItemDefs) / sizeof(kItemDefs[0]));

inline constexpr int kSlotCount = static_cast<int>(SlotType::Count);
inline constexpr int kBagCapacity = 15;  // 5 x 3 grid

// A bag entry: one item instance (or a stack of a consumable).
struct BagEntry {
    int defId = -1;   // index into kItemDefs, -1 = empty cell
    int count = 1;
};

inline constexpr int kNoItem = -1;

struct State {
    std::array<BagEntry, kBagCapacity> bag{};          // fixed grid order
    std::array<int, kSlotCount> equipped{};            // bag cell index or kNoItem

    State()
    {
        equipped.fill(kNoItem);
        // Preset loadout: two weapons, two armors, one helmet, potions.
        bag[0] = {0, 1};   // 铁剑
        bag[1] = {1, 1};   // 长弓
        bag[2] = {2, 1};   // 铁盔
        bag[3] = {3, 1};   // 皮甲
        bag[4] = {4, 1};   // 法袍
        bag[5] = {5, 3};   // 药水 x3
    }

    const ItemDef* DefOf(int bagIndex) const
    {
        if (bagIndex < 0 || bagIndex >= kBagCapacity) {
            return nullptr;
        }
        if (bag[bagIndex].defId < 0 || bag[bagIndex].defId >= kItemDefCount) {
            return nullptr;
        }
        return &kItemDefs[bag[bagIndex].defId];
    }

    // The equipment slot this bag cell would go into; kNoItem if the entry
    // is empty or non-equippable.
    int TargetSlot(int bagIndex) const
    {
        const ItemDef* def = DefOf(bagIndex);
        if (!def || def->slot == SlotType::Count) {
            return kNoItem;
        }
        return static_cast<int>(def->slot);
    }

    // Tap on a bag cell: equip equippables (swapping with whatever occupies
    // the slot), consume one use of consumables.
    void TapBag(int bagIndex)
    {
        const ItemDef* def = DefOf(bagIndex);
        if (!def) {
            return;
        }
        if (def->slot == SlotType::Count) {
            if (bag[bagIndex].count > 0) {
                --bag[bagIndex].count;
                if (bag[bagIndex].count == 0) {
                    bag[bagIndex].defId = -1;
                }
            }
            return;
        }
        const int slot = static_cast<int>(def->slot);
        const int previous = equipped[slot];
        equipped[slot] = bagIndex;
        if (previous != kNoItem && previous != bagIndex) {
            bag[bagIndex] = bag[previous];  // displaced item drops into this cell
        }
        bag[bagIndex].defId = -1;
        bag[bagIndex].count = 0;
    }

    // Tap on an equipment slot: unequip back into the first free bag cell.
    void TapEquip(int slot)
    {
        if (slot < 0 || slot >= kSlotCount) {
            return;
        }
        const int bagIndex = equipped[slot];
        if (bagIndex == kNoItem) {
            return;
        }
        for (int cell = 0; cell < kBagCapacity; ++cell) {
            if (bag[cell].defId < 0) {
                bag[cell] = bag[bagIndex];
                bag[bagIndex].defId = -1;
                bag[bagIndex].count = 0;
                equipped[slot] = kNoItem;
                return;
            }
        }
        // Bag full: keep the item equipped.
    }
};

}  // namespace inventory
