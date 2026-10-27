#include "gameplay/save_store.h"
#include <filesystem>
#include <limits>
#include <stdexcept>

static int checks=0;
static void Check(bool ok,const char* message) {++checks;if(!ok)throw std::runtime_error(message);}
int main() {
    namespace fs=std::filesystem;
    auto dir=fs::temp_directory_path()/("mikan-save-test-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    fs::create_directories(dir);
    auto root=dir.generic_string()+"/";
    saves::Store store;store.Open(root);Check(store.records.empty(),"fresh install must have no saves");
    saves::Record first;first.id=store.NextId();first.savedAt=1700000000;first.gold=35;
    first.inventory.EquipPistol();first.inventory.EquipSword();first.weapon=1;
    Check(store.Put(first),"create first slot");
    saves::Record second=first;second.id=store.NextId();second.savedAt+=100;
    second.scene="scenes/physics_parkour.json";second.x=-1;second.y=13.88f;second.z=-81.8f;
    second.checkpoint=5;second.gateOpen=true;second.gold=90;second.ammo=3;
    Check(store.Put(second),"create independent second slot");
    saves::Store reopened;reopened.Open(root);
    Check(reopened.records.size()==2,"both slots survive process restart");
    Check(reopened.records[0].id==second.id,"newest first");
    Check(reopened.records[0].gateOpen && reopened.records[0].checkpoint==5,"course progress restored");
    Check(std::fabs(reopened.records[0].z+81.8f)<.0001f,"save point coordinate round trip");
    Check(reopened.records[0].inventory.HasPistol() && reopened.records[0].inventory.HasSword(),"independent weapon slots restored");
    Check(reopened.records[1].gold==35,"creating another slot does not overwrite first");
    second.savedAt+=100;second.gold=120;Check(reopened.Put(second),"overwrite active slot only");
    reopened.Open(root);Check(reopened.records.size()==2&&reopened.records[0].gold==120,"update does not duplicate slot");
    // Sequence 3 is bank 1. Simulate an interrupted write in the newest bank.
    {std::ofstream broken(root+"saves1.dat",std::ios::trunc);broken<<"MIKAN_SAVES_V1 3 0\npartial";}
    reopened.Open(root);
    Check(reopened.records.size()==2&&reopened.records[0].gold==90,"truncated write falls back to complete bank");
    Check(reopened.Put(second),"recover by committing a new valid bank");
    for(int i=0;i<8;++i) {auto r=first;r.id=reopened.NextId();r.savedAt+=1000+i;Check(reopened.Put(r),"create multiple pages");}
    reopened.Open(root);Check(reopened.records.size()==10,"all pages survive restart");
    auto invalid=first;invalid.scene="../escape";Check(!reopened.Put(invalid),"reject untrusted scene path");
    invalid=first;invalid.health=std::numeric_limits<float>::quiet_NaN();Check(!reopened.Put(invalid),"reject non-finite health");
    invalid=first;invalid.inventory.equipped[0]=5;Check(!reopened.Put(invalid),"reject weapon in wrong equipment slot");
    invalid=first;invalid.ammo=-1;Check(!reopened.Put(invalid),"reject invalid ammo");
    Check(!saves::Date(first.savedAt).empty(),"last save timestamp available");
    saves::Store unwritable;unwritable.Open(root+"missing/");Check(!unwritable.Put(first),"failed write reports failure");
    Check(unwritable.records.empty(),"failed write does not mutate in-memory slots");
    std::printf("save_store_tests: %d checks passed\n",checks);
    return 0;
}
