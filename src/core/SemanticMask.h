#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "SemGroups.h"

namespace DXL {
// Coverage is inverse opacity: 0 = selected object, 255 = background.
// The image pair, dimensions and timestamp are published as one snapshot.
struct SemanticMaskSnapshot {
    std::vector<uint8_t> coverage, groupIds;
    uint32_t width = 0, height = 0;
    bool sourceFlipY = false; // Coverage/groupIds are always in source texture coordinates.
    uint64_t version = 0, publishedMs = 0;
    bool Valid(uint64_t nowMs) const noexcept {
        return width && height && version && nowMs >= publishedMs &&
            nowMs - publishedMs <= 500 && coverage.size() == size_t(width) * height &&
            groupIds.size() == coverage.size();
    }
};
inline float MaskStrength(float v) noexcept {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
}
inline uint8_t MaskByte(float v) noexcept {
    return uint8_t(MaskStrength(v) * 255.0f + 0.5f);
}

inline uint32_t SemanticSourceRow(uint32_t y, uint32_t height, bool flipY) noexcept {
    return flipY ? height - 1 - y : y;
}
// Called once on the upright model output, before publishing an immutable pair.
inline void RestoreSemanticSourceRows(SemanticMaskSnapshot& mask) noexcept {
    if (!mask.sourceFlipY) return;
    for (uint32_t y = 0; y < mask.height / 2; ++y)
        for (uint32_t x = 0; x < mask.width; ++x) {
            const size_t a = size_t(y)*mask.width+x;
            const size_t b = size_t(mask.height-1-y)*mask.width+x;
            std::swap(mask.coverage[a], mask.coverage[b]);
            std::swap(mask.groupIds[a], mask.groupIds[b]);
        }
}

// Compose continuous strength BEFORE filtering; never interpolate categorical IDs.
// Feather works on a <=640 long-edge grid, avoiding a full-screen multi-tap blur.
// R is semantic strength; G/B/A always retain the exact background byte.
// displayFlipY is only for the preview. The actual NR upload uses source coordinates.
inline void ComposeSemanticMask(uint8_t* dst, uint32_t pitch, uint32_t width,
    uint32_t height, const SemanticMaskSnapshot* mask, float background,
    const float* strengths, uint32_t enabledGroups, float feather = 0.0f,
    bool displayFlipY = false) noexcept {
    if (!dst || !width || !height) return;
    const float bg = MaskStrength(background);
    const uint8_t bgByte = MaskByte(bg);
    if (mask && (!mask->width || !mask->height ||
        mask->coverage.size()!=size_t(mask->width)*mask->height ||
        mask->groupIds.size()!=mask->coverage.size())) mask=nullptr;
    if (!mask) {
        for (uint32_t y=0;y<height;++y)
            std::fill_n(dst+size_t(y)*pitch, size_t(width)*4, bgByte);
        return;
    }
    // Allocation failures in a noexcept render callback must not terminate a game.
    try {
        const float gridScale=std::min(1.0f,640.0f/float(std::max(mask->width,mask->height)));
        const uint32_t w=std::max(1u,uint32_t(mask->width*gridScale+.5f));
        const uint32_t h=std::max(1u,uint32_t(mask->height*gridScale+.5f));
        auto strengthAt=[&](uint32_t x,uint32_t y) {
            const size_t i=size_t(y)*mask->width+x;
            const uint8_t group=mask->groupIds[i];
            if (group>=SEM_GROUP_COUNT || !(enabledGroups & (1u<<group))) return bg;
            const float coverage=1.0f-float(mask->coverage[i])/255.0f;
            return bg+(MaskStrength(strengths[group])-bg)*coverage;
        };
        std::vector<float> field(size_t(w)*h);
        for (uint32_t y=0;y<h;++y) {
            const float sy=std::clamp((float(y)+.5f)*mask->height/h-.5f,0.0f,float(mask->height-1));
            const uint32_t y0=uint32_t(sy),y1=std::min(y0+1,mask->height-1);
            const float fy=sy-y0;
            for (uint32_t x=0;x<w;++x) {
                const float sx=std::clamp((float(x)+.5f)*mask->width/w-.5f,0.0f,float(mask->width-1));
                const uint32_t x0=uint32_t(sx),x1=std::min(x0+1,mask->width-1);
                const float fx=sx-x0;
                const float a=strengthAt(x0,y0),b=strengthAt(x1,y0);
                const float c=strengthAt(x0,y1),d=strengthAt(x1,y1);
                field[size_t(y)*w+x]=(a+(b-a)*fx)*(1-fy)+(c+(d-c)*fx)*fy;
            }
        }
        feather=std::isfinite(feather)?std::clamp(feather,0.0f,8.0f):2.0f;
        if (feather>0.0f) {
            const int radius=int(std::ceil(feather));
            const float sigma=std::max(.35f,feather*.5f);
            float weights[17]{},sum=0;
            for (int k=-radius;k<=radius;++k) {
                const float weight=std::exp(-float(k*k)/(2*sigma*sigma));
                weights[k+radius]=weight;sum+=weight;
            }
            for (int i=0;i<=radius*2;++i) weights[i]/=sum;
            std::vector<float> temp(field.size());
            for (uint32_t y=0;y<h;++y) for (uint32_t x=0;x<w;++x) {
                float v=0;
                for (int k=-radius;k<=radius;++k)
                    v+=field[size_t(y)*w+std::clamp(int(x)+k,0,int(w)-1)]*weights[k+radius];
                temp[size_t(y)*w+x]=v;
            }
            for (uint32_t y=0;y<h;++y) for (uint32_t x=0;x<w;++x) {
                float v=0;
                for (int k=-radius;k<=radius;++k)
                    v+=temp[size_t(std::clamp(int(y)+k,0,int(h)-1))*w+x]*weights[k+radius];
                field[size_t(y)*w+x]=v;
            }
        }
        struct Column { uint32_t a,b;float f; };
        std::vector<Column> columns(width);
        for (uint32_t x=0;x<width;++x) {
            const float s=std::clamp((float(x)+.5f)*w/width-.5f,0.0f,float(w-1));
            const uint32_t a=uint32_t(s);
            columns[x]={a,std::min(a+1,w-1),s-a};
        }
        std::vector<float> line(w);
        for (uint32_t y=0;y<height;++y) {
            const uint32_t sampleY=SemanticSourceRow(y,height,displayFlipY);
            const float s=std::clamp((float(sampleY)+.5f)*h/height-.5f,0.0f,float(h-1));
            const uint32_t a=uint32_t(s),b=std::min(a+1,h-1);
            const float fy=s-a;
            for (uint32_t x=0;x<w;++x)
                line[x]=field[size_t(a)*w+x]*(1-fy)+field[size_t(b)*w+x]*fy;
            auto* row=dst+size_t(y)*pitch;
            for (uint32_t x=0;x<width;++x) {
                const auto& c=columns[x];
                row[x*4]=MaskByte(line[c.a]+(line[c.b]-line[c.a])*c.f);
                row[x*4+1]=row[x*4+2]=row[x*4+3]=bgByte;
            }
        }
    } catch (...) {
        for (uint32_t y=0;y<height;++y)
            std::fill_n(dst+size_t(y)*pitch,size_t(width)*4,bgByte);
    }
}
}
