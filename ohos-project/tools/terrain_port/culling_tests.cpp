#include "terrain/frustum_culling.h"
#include <cstdio>
#include <cstdlib>
#include <random>
static int checks=0;
void Check(bool ok) { ++checks; if(!ok) { std::fprintf(stderr,"FAIL at %d\n",checks); std::exit(1); } }
terrain::Bounds Box(float x,float y,float z,float h) {
    terrain::Bounds b; float p[3]={x-h,y-h,z-h}; b.Add(p);
    float q[3]={x+h,y+h,z+h}; b.Add(q); return b;
}
// Independent clip-space corner oracle, including the same world tolerance.
bool Oracle(const float* m,bool vk,const terrain::Bounds& b) {
    for(int plane=0;plane<6;++plane) {
        float best=-INFINITY;
        for(int corner=0;corner<8;++corner) {
            float v[4]={b.minimum[0],b.minimum[1],b.minimum[2],1};
            for(int i=0;i<3;++i) if(corner&(1<<i)) v[i]=b.maximum[i];
            float c[4]={}; for(int row=0;row<4;++row) for(int col=0;col<4;++col) c[row]+=m[4*col+row]*v[col];
            float d=plane==0?c[3]+c[0]:plane==1?c[3]-c[0]:plane==2?c[3]+c[1]:plane==3?c[3]-c[1]:plane==4?(vk?c[2]:c[3]+c[2]):c[3]-c[2];
            best=std::max(best,d);
        }
        if(best<0) return false;
    } return true;
}
int main() {
    float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    terrain::Frustum gl(identity,false),vk(identity,true);
    Check(gl.Intersects(Box(0,0,-0.5f,0.1f)));
    Check(!vk.Intersects(Box(0,0,-0.5f,0.1f)));
    Check(gl.Intersects(Box(1,0,0,0.1f)));
    Check(!gl.Intersects(Box(2,0,0,0.1f)));
    Check(gl.Intersects(Box(0,0,0,4)));
    Check(gl.Intersects(terrain::Bounds{}));
    std::mt19937 rng(91); std::uniform_real_distribution<float> d(-8,8), h(0.01f,2);
    for(bool zeroToOne : {false,true}) for(int matrix=0;matrix<20;++matrix) {
        float m[16];for(float& v:m) v=d(rng);
        terrain::Frustum f(m,zeroToOne);
        for(int i=0;i<1000;++i) {
            auto b=Box(d(rng),d(rng),d(rng),h(rng));
            // Conservative tolerance may retain a boundary box, never reject a visible box.
            Check(!Oracle(m,zeroToOne,b)||f.Intersects(b));
        }
    }
    float map[16]={1.f/30,0,0,0,0,1.f/30,0,0,0,0,1.f/30,0,0,0,0,1};
    terrain::Frustum f(map,false); int visible=0;
    for(int z=0;z<8;++z) for(int x=0;x<8;++x) {
        terrain::Bounds b; float p[3]={-96.f+24*x,-6.5f,-96.f+24*z}; b.Add(p);
        float q[3]={p[0]+24,3,p[2]+24};b.Add(q);visible+=f.Intersects(b);
    }
    Check(visible==16);
    float move[16]={1,0,0,0,0,1,0,0,0,0,1,0,3.5f,0,0,1};
    Check(!gl.Intersects(terrain::TransformBounds(terrain::ActorBounds(false),move)));
    Check(gl.Intersects(terrain::TransformBounds(terrain::ActorBounds(true),move)));
    // Rotated, translated, non-uniformly scaled static meshes: transforming
    // bounds conservatively must retain every clip-space-visible source box.
    for(int i=0;i<10000;++i) {
        const float angle=d(rng), sx=h(rng), sy=h(rng), sz=h(rng);
        float world[16]={std::cos(angle)*sx,0,-std::sin(angle)*sx,0,0,sy,0,0,
            std::sin(angle)*sz,0,std::cos(angle)*sz,0,d(rng),d(rng),d(rng),1};
        const auto local=Box(0,0,0,h(rng));
        Check(!Oracle(world,false,local) || gl.Intersects(terrain::TransformBounds(local,world)));
    }
    std::printf("PASS: %d culling checks; sample terrain view submits %d/64 chunks\n",checks,visible);
}
