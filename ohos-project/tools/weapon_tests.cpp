#include "inventory.h"
#include "inventory_ui.h"
#include "weapon_gameplay.h"
#include <cassert>
#include <cstdio>

int main() {
    inventory::State bag;
    assert(bag.SwordInBag() && !bag.HasSword());
    assert(bag.TapBag(0) && bag.HasPistol());
    bag.EquipSword();
    assert(bag.HasSword() && bag.HasPistol());
    assert(!bag.SwordInBag() && !bag.PistolInBag());
    assert(std::fabs(bag.Stats(5).attack-1.6f)<1e-5f);
    assert(std::fabs(bag.Stats(0).attack-1.3f)<1e-5f);
    assert(std::fabs(bag.Stats().attack-1.f)<1e-5f);
    bag.EquipSword(); bag.EquipPistol();
    assert(bag.HasSword() && bag.HasPistol());
    assert(bag.TapEquip(3) && !bag.HasSword() && bag.HasPistol());
    bag.EquipSword();
    assert(bag.TapEquip(0) && bag.HasSword() && !bag.HasPistol());
    bag.EquipPistol();
    for(float aspect:{4.f/3.f,16.f/9.f,20.f/9.f,2412.f/1084.f}) {
        float h=1084,w=h*aspect;
        gameplay::WeaponButton buttons[10];
        buttons[0]=gameplay::SwordButton(w,h);
        buttons[1]=gameplay::PotionButton(w,h);
        for(int i=0;i<4;i++)buttons[2+i]=gameplay::PistolButton(i,w,h);
        for(int i=0;i<4;i++)buttons[6+i]=gameplay::ActionButton(i,w,h);
        const auto attack=gameplay::ActionButton(0,w,h);
        for(int i=1;i<4;i++) {
            auto secondary=gameplay::ActionButton(i,w,h);
            float dx=secondary.x-attack.x,dy=secondary.y-attack.y;
            assert(std::fabs(std::sqrt(dx*dx+dy*dy)-h*.22f)<.01f);
        }
        // Health is beside the minimap; no bottom-centre quick slot
        // may cover its bar or steal a chest-interaction tap.
        for(int i=0;i<3;i++) {
            const auto& b=buttons[i];
            assert(b.y-b.r>h*.11f);
            assert(!gameplay::HitChestButton(b.x,b.y,w,h));
        }
        for(int i=1;i<=inventory::kSlotCount;i++) {
            auto a=inventory_ui::Layout(i,w,h);
            for(int j=i+1;j<=inventory::kSlotCount;j++) {
                auto b=inventory_ui::Layout(j,w,h);
                assert(std::fabs(a.cx-b.cx)>a.hw+b.hw);
            }
        }
        for(int i=0;i<10;i++) {
            const auto& a=buttons[i];
            assert(a.x-a.r>0 && a.x+a.r<w && a.y-a.r>0 && a.y+a.r<h);
            for(int j=i+1;j<10;j++) {
                const auto& b=buttons[j];
                const float dx=a.x-b.x,dy=a.y-b.y;
                assert(dx*dx+dy*dy>(a.r+b.r+10)*(a.r+b.r+10));
            }
        }
    }
    auto b = gameplay::SwordButton(1920,1080);
    assert(gameplay::HitSwordButton(b.x,b.y,1920,1080));
    assert(!gameplay::HitSwordButton(b.x+b.r+1,b.y,1920,1080));
    gameplay::PistolState gun;
    gun.Button(0); gun.Button(2); gun.Tick(.01f,true);
    assert(gun.fired && gun.ammo==11);
    gun.Holster(); gun.Button(2); gun.Tick(1,true);
    assert(!gun.fired && gun.ammo==11);
    float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    float mount[16];
    gameplay::SwordMount(identity,identity,identity,mount);
    // The baked +Y blade grip must land at the calibrated palm.
    const float palm[3]={.0008136f,.096766587f,.027578364f};
    for(int row=0;row<3;++row)
        assert(std::fabs(mount[4+row]*(-.675f)+mount[12+row]-palm[row])<1e-5f);
    assert(gameplay::RayEnemy(0,0,5,0,0,-1,0,0,0)<5);
    assert(gameplay::RayEnemy(5,0,5,0,0,-1,0,0,0)==60);
    std::puts("weapon tests passed");
}
