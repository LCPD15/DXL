#include "SemanticMask.h"
#include <cstdio>
#include <stdexcept>
#include <chrono>
using namespace DXL;
void Check(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
int main() try {
    float strengths[SEM_GROUP_COUNT]{};
    SemanticMaskSnapshot m;
    m.width=4;m.height=4;m.coverage.assign(16,0);m.groupIds.assign(16,0);
    // Upright model output: top selected, bottom background, asymmetric group IDs.
    for(unsigned i=8;i<16;++i){m.coverage[i]=255;m.groupIds[i]=17;}
    const auto original=m;
    m.sourceFlipY=true;RestoreSemanticSourceRows(m);
    Check(m.coverage[0]==255 && m.groupIds[0]==17 && m.coverage[12]==0 && m.groupIds[12]==0,"mask pair not restored to source rows");
    uint8_t raw[4*4*4]{},preview[4*4*4]{};
    ComposeSemanticMask(raw,16,4,4,&m,1,strengths,1,0);
    ComposeSemanticMask(preview,16,4,4,&m,1,strengths,1,0,true);
    Check(raw[0]==255 && raw[48]==0,"NR mask must retain upside-down source coordinates");
    Check(preview[0]==0 && preview[48]==255,"display preview must be upright");
    RestoreSemanticSourceRows(m);
    Check(m.coverage==original.coverage && m.groupIds==original.groupIds,"flip pair round trip");
    m.sourceFlipY=false;
    m.width=32;m.height=16;m.coverage.assign(512,0);m.groupIds.assign(512,0);
    for(unsigned y=0;y<16;++y) for(unsigned x=16;x<32;++x)m.groupIds[y*32+x]=255;
    std::vector<uint8_t> soft(512*4),hard(512*4),fine(128*64*4);
    ComposeSemanticMask(hard.data(),128,32,16,&m,1,strengths,1,0);
    ComposeSemanticMask(soft.data(),128,32,16,&m,1,strengths,1,3);
    Check(hard[15*4]==0 && hard[16*4]==255,"hard baseline");
    Check(soft[15*4]>0 && soft[15*4]<128 && soft[16*4]>127 && soft[16*4]<255,"missing feather transition");
    Check(soft[0]==0 && soft[31*4]==255,"feather altered distant interior");
    for(unsigned x=1;x<32;++x)Check(soft[x*4]>=soft[(x-1)*4],"edge blur introduced ringing");
    for(unsigned i=0;i<512;++i)for(unsigned c=1;c<4;++c)Check(soft[i*4+c]==255,"GBA background changed");
    ComposeSemanticMask(fine.data(),128*4,128,64,&m,1,strengths,1,0);
    Check(fine[62*4]>0 && fine[62*4]<255,"mask upsample still nearest-neighbor");
    ComposeSemanticMask(soft.data(),128,32,16,&m,0,strengths,1,8);
    for(auto v:soft)Check(v==0,"all-zero mask must be exact zero");
    strengths[0]=1;ComposeSemanticMask(soft.data(),128,32,16,&m,1,strengths,1,8);
    for(auto v:soft)Check(v==255,"all-one mask must be exact one");
    strengths[0]=0;ComposeSemanticMask(soft.data(),128,32,16,&m,.5f,strengths,0,4);
    for(auto v:soft)Check(v==128,"disabled group must preserve exact background");
    ComposeSemanticMask(soft.data(),128,32,16,nullptr,.5f,strengths,1,4);
    for(auto v:soft)Check(v==128,"stale fallback changed");
    puts("PASS source/model/preview orientation, group-pair restore, soft edges, interpolation, exact GBA, zero/one/disabled/stale");
    m.width=640;m.height=360;m.coverage.assign(640*360,0);m.groupIds.assign(640*360,0);
    for(unsigned y=0;y<m.height;++y) for(unsigned x=320;x<640;++x)m.groupIds[y*640+x]=255;
    std::vector<uint8_t> out(640*360*4);
    for(float feather:{0.0f,2.0f,8.0f}) {
        ComposeSemanticMask(out.data(),640*4,640,360,&m,1,strengths,1,feather);
        auto a=std::chrono::steady_clock::now();
        for(int i=0;i<5;++i)ComposeSemanticMask(out.data(),640*4,640,360,&m,1,strengths,1,feather);
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-a).count()/5;
        printf("640-grid CPU mask composition (GPU upsamples to 4K) feather=%.0f %.3f ms/update (not per game frame)\n",feather,ms);
    }
    return 0;
} catch(const std::exception& e) {printf("FAIL %s\n",e.what());return 1;}
