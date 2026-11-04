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
    {
        std::ifstream bank(root+"saves1.dat",std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(bank)),{});
        std::istringstream body(text.substr(text.find('\n')+1));
        std::string count,line;std::getline(body,count);std::getline(body,line);
        for(int field=0;field<8;++field)line.resize(line.rfind(' '));
        std::string oldBody=count+"\n"+line+"\n";
        unsigned hash=2166136261u;for(unsigned char c:oldBody)hash=(hash^c)*16777619u;
        fs::create_directory(dir/"legacy");
        {std::ofstream old(root+"legacy/saves1.dat",std::ios::binary);old<<"MIKAN_SAVES_V1 1 "<<hash<<"\n"<<oldBody;}
        const std::string v2Body=count+"\n"+line+" 0 0\n";
        unsigned v2Hash=2166136261u;for(unsigned char c:v2Body)v2Hash=(v2Hash^c)*16777619u;
        fs::create_directory(dir/"legacy2");
        {std::ofstream old(root+"legacy2/saves1.dat",std::ios::binary);old<<"MIKAN_SAVES_V2 1 "<<v2Hash<<"\n"<<v2Body;}
        saves::Store legacy2;legacy2.Open(root+"legacy2/");
        Check(legacy2.records.size()==1 && legacy2.records[0].inventory.growth.level==1 && legacy2.records[0].inventory.growth.points==0,"V2 saves migrate to level one with no allocated points");
        saves::Store legacy;legacy.Open(root+"legacy/");
        Check(legacy.records.size()==1 && legacy.records[0].gold==35 && legacy.records[0].courseRevision==0,"existing V1 saves remain readable");
    }
    saves::Record second=first;second.id=store.NextId();second.savedAt+=100;
    second.scene="scenes/physics_parkour.json";second.x=-1;second.y=13.28f;second.z=-81.3f;second.parkourRewardClaimed=true;second.courseRevision=3;
    second.inventory.growth.Gain(160);second.inventory.growth.Spend(0);
    second.checkpoint=6;second.gateOpen=true;second.gold=90;second.ammo=3;
    Check(store.Put(second),"create independent second slot");
    saves::Store reopened;reopened.Open(root);
    Check(reopened.records.size()==2,"both slots survive process restart");
    Check(reopened.records[0].id==second.id,"newest first");
    Check(reopened.records[0].inventory.growth.level==3 && reopened.records[0].inventory.growth.points==5 && reopened.records[0].inventory.growth.strength==1,"progression and allocated points survive restart");
    Check(reopened.records[0].gateOpen && reopened.records[0].checkpoint==6,"course progress restored");
    Check(std::fabs(reopened.records[0].z+81.3f)<.0001f,"save point coordinate round trip");
    Check(reopened.records[0].inventory.HasPistol() && reopened.records[0].inventory.HasSword(),"independent weapon slots restored");
    Check(reopened.records[1].gold==35,"creating another slot does not overwrite first");
    Check(reopened.records[0].parkourRewardClaimed && reopened.records[0].courseRevision==3,"saved reward claim and layout revision survive restart");
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
    const auto removed=reopened.records.front().id;
    Check(reopened.Remove(removed),"delete chosen slot");
    reopened.Open(root);
    Check(reopened.records.size()==9,"delete persists after restart");
    Check(std::none_of(reopened.records.begin(),reopened.records.end(),[removed](const saves::Record& r){return r.id==removed;}),"deleted slot stays absent");
    Check(!reopened.Remove(removed),"missing slot deletion fails without mutation");
    while(!reopened.records.empty())Check(reopened.Remove(reopened.records.back().id),"delete remaining slots");
    reopened.Open(root);Check(reopened.records.empty(),"deleting last slot persists empty list");
    progression::State growth;Check(growth.Valid(),"new progression valid");
    Check(!growth.Spend(0),"cannot spend unearned points");growth.Gain(59);Check(growth.level==1,"below threshold");growth.Gain(1);Check(growth.level==2&&growth.points==3&&growth.xp==0,"level up grants exactly three points");
    Check(growth.Spend(2)&&growth.agility==1&&growth.points==2&&growth.Valid(),"spending preserves point budget");
    Check(!growth.Spend(3),"reject unknown stat");growth.Gain(1000000);Check(growth.level==50&&growth.xp==0&&growth.Valid(),"multi-level experience respects level cap");
    auto invalidGrowth=first;invalidGrowth.inventory.growth.strength=10;Check(!reopened.Put(invalidGrowth),"reject invented attribute points");
    std::printf("save_store_tests: %d checks passed\n",checks);
    return 0;
}
