#pragma once
#include "wavelet_rate_control.hpp"
#include "temporal_coefficients.hpp"
#include <array>

namespace fajk {
// Ordered producer-owned history. The native quantizer is read from the
// selected packet, not the last discarded rate/geometry trial.
class ClipRateControl {
    std::array<uint32_t,48> histogram_{};
    uint32_t width_=0,height_=0,nativeWidth_=0,nativeHeight_=0;
    float step_=0;size_t bytes_=0;
public:
    WaveletRateControl begin(const Frame& frame,const EncodeOptions& options){
        frame.validate();
        std::array<uint32_t,48> histogram{};uint64_t samples=0;
        for(uint32_t y=std::min(8u,frame.height-1);y<frame.height;y+=32)for(uint32_t x=std::min(8u,frame.width-1);x<frame.width;x+=32){
            const auto at=(size_t(y)*frame.width+x)*4;for(unsigned c=0;c<3;++c)++histogram[c*16+frame.pixels[at+c]/16];++samples;
        }
        uint64_t difference=0;for(size_t i=0;i<histogram.size();++i)difference+=uint64_t(std::abs(int64_t(histogram[i])-histogram_[i]));
        const bool cut=frame.width!=width_||frame.height!=height_||difference>samples*3;
        width_=frame.width;height_=frame.height;histogram_=histogram;
        if(cut)step_=0;
        WaveletRateControl result;result.preferredStep=step_;result.minimumFill=fastEncode(options.speed)?.97f:.995f;
        const auto target=options.targetBytes?options.targetBytes:qualityTargetBytes(frame.width,frame.height,options.quality);
        if(bytes_&&bytes_*100< uint64_t(target)*97)result.preferredStep*=.995f;
        return result;
    }
    void finish(const Packet& packet){
        const auto info=coefficientEncodingInfo(packet);bytes_=packet.data.size();
        if(!info){step_=0;nativeWidth_=nativeHeight_=0;return;}
        const bool changed=nativeWidth_&&(nativeWidth_!=info->width||nativeHeight_!=info->height);
        nativeWidth_=info->width;nativeHeight_=info->height;step_=changed?0:info->step;
    }
};
}
