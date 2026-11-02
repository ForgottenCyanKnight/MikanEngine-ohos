#include "gameplay/arena_waves.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
static int checks=0;
static void Check(bool result,const char* message) {++checks;if(!result){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
int main() {
    gameplay::ArenaWaves waves;
    Check(waves.wave==0&&!waves.complete,"fresh visit begins before first wave");
    Check(!waves.Update(std::numeric_limits<float>::quiet_NaN(),0,0),"ignore invalid frame time");
    int total=0;
    for(int expected=1;expected<=4;++expected) {
        int count=0;
        for(int frame=0;frame<200&&!count;++frame)count=waves.Update(1.f/60,0,0);
        Check(count==expected,"wave spawns exactly 1,2,3,4 enemies");total+=count;
        for(int frame=0;frame<400;++frame)Check(!waves.Update(1.f/60,1,expected),"a surviving enemy prevents next wave");
        Check(waves.wave==expected,"survivors cannot skip a wave");
        for(int frame=0;frame<200;++frame)Check(!waves.Update(1.f/60,0,1),"death/dissolve poses remain before recycling actors");
        Check(waves.wave==expected,"corpse animation cannot trigger early wave");
    }
    Check(total==10,"four waves contain ten enemies in total");
    Check(!waves.Update(1.f/60,0,0)&&waves.complete,"last clear completes arena");
    for(int i=0;i<200;++i)Check(!waves.Update(.1f,0,0)&&waves.wave==4,"no fifth wave");
    waves={};Check(waves.wave==0&&!waves.complete&&waves.countdown==3,"retry/new visit resets all wave progress");
    std::printf("Arena wave tests passed: %d checks, waves 1/2/3/4 (10 enemies).\n",checks);
}
