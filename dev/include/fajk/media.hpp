#pragma once
#include "types.hpp"
#include <memory>
namespace fajk {
class MediaRuntime {
public:
    MediaRuntime();~MediaRuntime();
    MediaRuntime(const MediaRuntime&)=delete;
};
struct MediaInfo {
    uint32_t width=0,height=0,fpsNum=30,fpsDen=1;
    int64_t duration=0;
    bool image=false;
};
class MediaSource {
public:
    explicit MediaSource(const Path& path,uint32_t maxWidth=0);
    ~MediaSource();
    const MediaInfo& info() const;
    bool next(Frame& frame);
    void seek(int64_t pts);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
