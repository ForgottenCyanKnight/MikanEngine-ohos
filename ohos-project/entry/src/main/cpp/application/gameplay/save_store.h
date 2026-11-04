#pragma once
#include "../inventory.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace saves {
struct Record {
    unsigned id=0;
    long long savedAt=0;
    std::string scene="scenes/main.json";
    float x=0,y=0,z=0,yaw=0,health=100,seconds=0;
    int gold=0,ammo=12,reserve=24,weapon=-1,checkpoint=0,falls=0;
    bool gateOpen=false,chestOpened=false,enemyDead=false;
    bool parkourRewardClaimed=false;
    int courseRevision=0;
    inventory::State inventory;
};
inline bool Valid(const Record& r) {
    if(!r.inventory.growth.Valid())return false;
    if (!r.id || r.savedAt<=0 || (r.scene!="scenes/main.json" &&
        r.scene!="scenes/forest_test.json" && r.scene!="scenes/physics_parkour.json")) return false;
    for(float f:{r.x,r.y,r.z,r.yaw,r.health,r.seconds}) if(!std::isfinite(f)||std::fabs(f)>1000000) return false;
    if(r.health<=0||r.health>100||r.gold<0||r.gold>10000000||r.ammo<0||r.ammo>12||
       r.reserve<0||r.reserve>1000000||r.weapon< -1||r.weapon>1||r.checkpoint<0||r.checkpoint>15||r.falls<0||r.seconds<0||r.courseRevision<0||r.courseRevision>100000) return false;
    for(const auto& b:r.inventory.bag)
        if(b.defId< -1||b.defId>=inventory::kItemDefCount||b.count<0||b.count>1000000||
           ((b.defId==-1)!=(b.count==0))) return false;
    for(int i=0;i<inventory::kSlotCount;++i) {
        int id=r.inventory.equipped[i];
        if(id< -1||id>=inventory::kItemDefCount || (id>=0&&int(inventory::kItemDefs[id].slot)!=i)) return false;
    }
    return true;
}
inline std::string Date(long long value) {
    std::time_t t=static_cast<std::time_t>(value);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local,&t);
#else
    localtime_r(&t,&local);
#endif
    char text[32]; std::strftime(text,sizeof(text),"%Y-%m-%d %H:%M:%S",&local); return text;
}
// Two complete banks: an interrupted write never replaces the last valid bank.
// Each bank has a checksum and monotonically increasing commit sequence.
class Store {
    std::string root_;
    unsigned long long sequence_=0;
    static unsigned Hash(const std::string& s) { unsigned h=2166136261u;for(unsigned char c:s) h=(h^c)*16777619u;return h; }
    static bool Decode(const std::string& file,unsigned long long& seq,std::vector<Record>& out) {
        std::ifstream f(file,std::ios::binary); if(!f) return false;
        std::string data((std::istreambuf_iterator<char>(f)),{});
        if(data.size()>1024*1024) return false;
        auto cut=data.find('\n');if(cut==std::string::npos)return false;
        std::istringstream head(data.substr(0,cut)); std::string magic; unsigned checksum;
        if(!(head>>magic>>seq>>checksum)||(magic!="MIKAN_SAVES_V1"&&magic!="MIKAN_SAVES_V2"&&magic!="MIKAN_SAVES_V3")||!seq)return false;
        auto body=data.substr(cut+1);if(Hash(body)!=checksum)return false;
        std::istringstream in(body);std::size_t count=0;if(!(in>>count)||count>1024)return false;
        out.clear();
        for(std::size_t i=0;i<count;++i) {
            Record r;
            if(!(in>>r.id>>r.savedAt>>r.scene>>r.x>>r.y>>r.z>>r.yaw>>r.health>>r.gold>>r.ammo>>r.reserve>>r.weapon>>r.checkpoint>>r.falls>>r.seconds>>r.gateOpen>>r.chestOpened>>r.enemyDead))return false;
            for(auto& b:r.inventory.bag) if(!(in>>b.defId>>b.count))return false;
            for(auto& e:r.inventory.equipped)if(!(in>>e))return false;
            if(magic!="MIKAN_SAVES_V1" && !(in>>r.parkourRewardClaimed>>r.courseRevision))return false;
            if(magic=="MIKAN_SAVES_V3"){auto& g=r.inventory.growth;if(!(in>>g.level>>g.xp>>g.points>>g.strength>>g.defense>>g.agility))return false;}
            if(!Valid(r)||std::any_of(out.begin(),out.end(),[&](const Record& old){return old.id==r.id;}))return false;
            out.push_back(r);
        }
        in>>std::ws; return in.eof();
    }
public:
    std::vector<Record> records;
    void Open(const std::string& root) {
        root_=root;records.clear();sequence_=0;
        for(int i=0;i<2;++i) {
            std::vector<Record> candidate;unsigned long long seq=0;
            if(Decode(root_+"saves"+std::to_string(i)+".dat",seq,candidate)&&seq>sequence_) {sequence_=seq;records=std::move(candidate);}
        }
        std::sort(records.begin(),records.end(),[](const Record& a,const Record& b){return a.savedAt>b.savedAt || (a.savedAt==b.savedAt&&a.id>b.id);});
    }
    unsigned NextId() const {unsigned id=1;for(const auto& r:records)id=std::max(id,r.id+1);return id;}
    bool Put(Record r) {
        if(root_.empty()||!Valid(r))return false;
        auto next=records;auto it=std::find_if(next.begin(),next.end(),[&](const Record& old){return old.id==r.id;});
        if(it==next.end()) {if(next.size()>=1024)return false;next.push_back(r);} else *it=r;
        return Commit(std::move(next));
    }
    bool Remove(unsigned id) {
        auto next=records;
        const auto it=std::find_if(next.begin(),next.end(),[id](const Record& r){return r.id==id;});
        if(it==next.end() || root_.empty())return false;
        next.erase(it);
        return Commit(std::move(next));
    }
private:
    bool Commit(std::vector<Record> next) {
        std::ostringstream out;out<<std::setprecision(9)<<next.size()<<'\n';
        for(const auto& v:next) {
            out<<v.id<<' '<<v.savedAt<<' '<<v.scene<<' '<<v.x<<' '<<v.y<<' '<<v.z<<' '<<v.yaw<<' '<<v.health<<' '<<v.gold<<' '<<v.ammo<<' '<<v.reserve<<' '<<v.weapon<<' '<<v.checkpoint<<' '<<v.falls<<' '<<v.seconds<<' '<<v.gateOpen<<' '<<v.chestOpened<<' '<<v.enemyDead;
            for(auto b:v.inventory.bag)out<<' '<<b.defId<<' '<<b.count;
            for(int e:v.inventory.equipped)out<<' '<<e;
            out<<' '<<v.parkourRewardClaimed<<' '<<v.courseRevision;
            const auto& g=v.inventory.growth;out<<' '<<g.level<<' '<<g.xp<<' '<<g.points<<' '<<g.strength<<' '<<g.defense<<' '<<g.agility;
            out<<'\n';
        }
        auto body=out.str();auto seq=sequence_+1;
        const std::string file=root_+"saves"+std::to_string(seq%2)+".dat";
        std::string bytes="MIKAN_SAVES_V3 "+std::to_string(seq)+" "+std::to_string(Hash(body))+"\n"+body;
        FILE* f=std::fopen(file.c_str(),"wb");if(!f)return false;
        bool ok=std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();
        ok=std::fflush(f)==0&&ok;
#ifndef _WIN32
        ok=::fsync(fileno(f))==0&&ok;
#else
        ok=FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))))&&ok;
#endif
        ok=std::fclose(f)==0&&ok;
        if(!ok)return false;
        sequence_=seq;records=std::move(next);return true;
    }
};
} // namespace saves
