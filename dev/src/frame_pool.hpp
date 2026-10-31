#pragma once
#include "fajk/types.hpp"
#include <array>
#include <memory>
#include <mutex>

namespace fajk {
// UI frames may outlive the decoder and be released on the UI thread.
// Only the final shared owner returns a frame; in-flight pixels are never reused.
class FramePool {
    struct State {
        std::mutex mutex;
        std::array<std::unique_ptr<Frame>, 2> idle;
        size_t retainedBytes=0;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
public:
    std::shared_ptr<Frame> acquire(uint32_t w, uint32_t h, int64_t pts, int64_t duration) {
        const auto size = pixelBytes(w,h);
        std::unique_ptr<Frame> frame;
        {
            std::lock_guard lock(state_->mutex);
            for(auto& cached:state_->idle)if(cached&&cached->pixels.size()==size){state_->retainedBytes-=cached->pixels.capacity();frame=std::move(cached);break;}
            if(!frame)for(auto& cached:state_->idle)if(cached){state_->retainedBytes-=cached->pixels.capacity();frame=std::move(cached);break;}
        }
        if(!frame)frame=std::make_unique<Frame>();
        frame->width=w;frame->height=h;frame->pts=pts;frame->duration=duration;
        frame->pixels.resize(size);
        return std::shared_ptr<Frame>(frame.release(),[state=state_](Frame* raw){
            std::unique_ptr<Frame> released(raw);
            constexpr size_t limit=64u*1024u*1024u;
            const auto bytes=released->pixels.capacity();if(bytes>limit)return;
            std::lock_guard lock(state->mutex);
            if(bytes>limit-state->retainedBytes)return;
            for(auto& cached:state->idle)if(!cached){state->retainedBytes+=bytes;cached=std::move(released);break;}
        });
    }
};
}
