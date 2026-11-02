#pragma once
#include <algorithm>
#include <cmath>
namespace gameplay {
// Four finite waves. Wait for death poses/dissolves before recycling actors.
struct ArenaWaves {
    int wave=0;
    float countdown=3;
    bool complete=false;
    int Update(float dt,int alive,int visible) {
        if(complete || alive>0 || visible>0 || !std::isfinite(dt) || dt<=0)return 0;
        if(wave==4) {complete=true;return 0;}
        countdown=std::max(0.f,countdown-std::min(dt,.1f));
        if(countdown>0)return 0;
        ++wave;countdown=3;return wave;
    }
};
}
