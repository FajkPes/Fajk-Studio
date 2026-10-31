#include "playback_decoder.hpp"
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/media.hpp"
#include "fajk/hardware.hpp"
#include "frame_pool.hpp"
#include "frame_time.hpp"
#include "sequence_decode_ahead.hpp"
#include "decode_throughput.hpp"
#include "temporal_coefficients.hpp"
#include "reconstruction441.hpp"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

namespace fajk {
namespace {
std::wstring errorText(const char* message){
    const int count=MultiByteToWideChar(CP_UTF8,0,message,-1,nullptr,0);if(count<=1)return L"Decode failed.";
    std::wstring result(size_t(count),0);MultiByteToWideChar(CP_UTF8,0,message,-1,result.data(),count);result.pop_back();return result;
}
bool parallelPlayback(const DecodeRequest& work,const SequenceInfo& info,const Packet& packet,uint32_t workers){
    if(!work.continuous||work.mode!=1||work.decodeLanes==1||workers<6||work.last<=work.frame)return false;
    if(work.decodeLanes!=2&&(work.last-work.frame+work.step-1)/work.step<4)return false;
    const auto hardware=hardwareInfo();const auto available=(work.memoryMiB?std::min(work.memoryMiB,hardware.safeMiB):hardware.safeMiB)*1048576;
    const auto pixels=uint64_t((info.width+63)&~63u)*((info.height+63)&~63u),raw=uint64_t(pixelBytes(info.width,info.height));
    // Two lane contexts plus the direct seek context, five admitted jobs and
    // the existing three UI-ready frames. The direct GPU context stays alive.
    // Use the same conservative reserve as conversion, counting it per context.
    const auto reserve=raw*8+pixels*24+128*1048576ull,job=uint64_t((info.width+63)&~63u)*256*80+4*1048576ull;
    if(available<reserve*3+job*workers)return false;
    if(work.decodeLanes==2)return true;
    if(work.backend!=ComputeBackend::Gpu||uint64_t(info.width)*info.height<512*512||uint64_t(info.width)*info.height>4*1024*1024)return false;
    const Packet* root=&packet;unsigned depth=0;while(root->reference&&depth++<coefficientMaxDepth)root=root->reference.get();
    if(root->reference||root->data.size()<5||root->data[4]!=130)return false;
    const auto reconstruction=read441(root->data,info.width,info.height);
    return !reconstruction.settings.protectText&&reconstruction.settings.intensity&&bool(coefficientEncodingInfo(*root));
}
}
struct PlaybackDecoder::Impl {
    std::mutex mutex;std::condition_variable wake;
    std::optional<DecodeRequest> request;std::deque<DecodeResult> decoded;
    std::atomic<uint64_t> epoch=0;bool closing=false;
    Path trace;std::wstring failure;std::unique_ptr<SequenceDecodeAhead> ahead;std::jthread worker;
    explicit Impl(Path path):trace(std::move(path)),worker([this]{
        try{run();}catch(const std::exception& error){fail(errorText(error.what()));}catch(...){fail(L"Playback initialization failed.");}
    }){}
    void fail(std::wstring error){
        std::lock_guard lock(mutex);failure=std::move(error);
        if(request){DecodeResult result;result.frame=request->frame;result.generation=request->generation;result.error=failure;decoded.push_back(std::move(result));request.reset();}
    }
    ~Impl(){shutdown();}
    void shutdown(){
        {std::lock_guard lock(mutex);closing=true;request.reset();++epoch;}wake.notify_one();
        if(worker.joinable())worker.join();
        if(ahead)ahead->cancel();
    }
    void run(){
        MediaRuntime runtime;std::unique_ptr<SequenceReader> reader;std::unique_ptr<MediaSource> source;
        Path sequence,sourceFile;uint64_t content=UINT64_MAX;
        auto codec=makeCodec(CodecId::Fajk);FramePool frames,sourceFrames;
        std::shared_ptr<Frame> last;std::optional<DecodeRequest> next;
        std::optional<uint64_t> aheadGeneration;
        bool aheadUnavailable=false;
        DecodeThroughput directThroughput;
        for(;;){
            DecodeRequest work;bool explicitRequest=false;
            {std::unique_lock lock(mutex);wake.wait(lock,[&]{return closing||request||(next&&(next->generation!=epoch||decoded.size()<3));});
                if(closing)return;
                if(next&&next->generation!=epoch)next.reset();
                if(request){work=std::move(*request);request.reset();decoded.clear();next.reset();explicitRequest=true;}
                else if(next){work=*next;next.reset();}else continue;
            }
            if(explicitRequest){if(ahead)ahead->cancel();aheadGeneration.reset();aheadUnavailable=false;directThroughput={};}
            DecodeResult result;result.frame=work.frame;result.generation=work.generation;result.filter=work.deblocking;result.sharpening=work.sharpening;
            try{
                const auto start=std::chrono::steady_clock::now();require(!work.path.empty()||!work.source.empty(),"Missing playback source.");require(work.step>0&&work.decodeLanes<=2,"Invalid playback scheduling.");
                if(content!=work.contentRevision){reader.reset();source.reset();last.reset();sequence.clear();sourceFile.clear();codec=makeCodec(CodecId::Fajk);content=work.contentRevision;}
                codec->setComputeBackend(work.backend);codec->setDeblocking(work.deblocking);codec->setSharpening(work.sharpening);
                codec->setCacheBudgetBytes(work.memoryMiB?size_t(std::min<uint64_t>(256,work.memoryMiB/8))*1024*1024:256u*1024u*1024u);
                auto target=work.pts;
                if(!work.path.empty()){
                    if(sequence!=work.path){reader=std::make_unique<SequenceReader>(work.path);sequence=work.path;
                        if(!trace.empty())std::ofstream(trace,std::ios::app)<<"sequence open content="<<content<<" request="<<work.generation<<'\n';}
                    require(work.frame<reader->info().count,"Playback frame index is out of range.");target=reader->index()[work.frame].pts;
                    if(work.mode!=0){
                        const auto& info=reader->info();
                        if(aheadGeneration&&*aheadGeneration==work.generation){
                            auto prepared=ahead->take(work.frame,work.generation,&epoch);
                            if(!prepared){ahead->cancel();aheadGeneration.reset();continue;}
                            require(prepared->error.empty(),prepared->error.c_str());result.image=std::move(prepared->image);result.decodeMs=prepared->decodeMs;result.lanes=2;
                            result.throughputMs=prepared->throughputMs;result.throughputFrames=prepared->throughputFrames;
                        }else{
                            const auto prepareStart=DecodeThroughput::Clock::now();
                            const auto packet=reader->read(work.frame);const auto workers=safeWorkers(info.width,info.height,false,work.threads,work.memoryMiB);codec->setThreads(workers);
                            const auto decodeStart=std::chrono::steady_clock::now();result.image=frames.acquire(info.width,info.height,packet.pts,packet.duration);
                            codec->decodeInto(packet,info.width,info.height,result.image->pixels);
                            result.decodeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-decodeStart).count();
                            const auto finished=DecodeThroughput::Clock::now();
                            directThroughput.begin(prepareStart);directThroughput.finish(finished);
                            const auto speed=directThroughput.sample(finished);result.throughputMs=speed.msPerFrame;result.throughputFrames=speed.frames;
                            // Keep the first frame and arbitrary seeks on the direct path.
                            // Only subsequent continuous frames use the parallel window.
                            if(!aheadUnavailable)try{if(parallelPlayback(work,info,packet,workers)){
                                if(!ahead)ahead=std::make_unique<SequenceDecodeAhead>(2);
                                SequenceDecodeSettings settings;settings.path=work.path;settings.width=info.width;settings.height=info.height;
                                settings.threads=workers;settings.memoryMiB=work.memoryMiB;settings.backend=work.backend;
                                settings.generation=work.generation;settings.contentRevision=work.contentRevision;settings.deblocking=work.deblocking;settings.sharpening=work.sharpening;
                                settings.epoch=&epoch;
                                settings.includeLast=true;
                                ahead->start(std::move(settings),work.frame+std::min<uint64_t>(work.step,work.last-work.frame),work.last,work.step,5,4);
                                aheadGeneration=work.generation;
                                if(!trace.empty())std::ofstream(trace,std::ios::app)<<"decode ahead lanes=2 window=5 chunk=4 workers="<<workers<<" generation="<<work.generation<<'\n';
                            }}catch(const std::exception& error){
                                if(ahead)ahead->cancel();aheadGeneration.reset();aheadUnavailable=true;
                                if(!trace.empty())std::ofstream(trace,std::ios::app)<<"decode ahead unavailable; direct fallback: "<<error.what()<<'\n';
                            }
                        }
                    }
                }
                if(!work.source.empty()&&(work.path.empty()||work.mode!=1)){
                    if(sourceFile!=work.source){source=std::make_unique<MediaSource>(work.source);sourceFile=work.source;last.reset();
                        if(!trace.empty())std::ofstream(trace,std::ios::app)<<"source open content="<<content<<" request="<<work.generation<<'\n';}
                    if(!last||target<last->pts||(target>=last->pts&&target-last->pts>10000000)){source->seek(target);last.reset();}
                    while(!last||last->pts+std::max<int64_t>(1,last->duration)<=target){
                        if(work.generation!=epoch)break;
                        auto frame=sourceFrames.acquire(source->info().width,source->info().height,0,1);
                        if(!source->next(*frame))break;last=std::move(frame);
                    }
                    require(bool(last),"Original frame could not be read.");result.reference=last;if(!result.image)result.image=last;
                }
                result.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            }catch(const std::exception& error){result.error=errorText(error.what());}catch(...){result.error=L"Snímek se nepodařilo načíst.";}
            {std::lock_guard lock(mutex);if(work.generation!=epoch||request||closing)continue;
                if(result.error.empty()&&work.continuous&&work.frame<work.last){
                    try{next=work;next->frame=work.frame+std::min<uint64_t>(work.step,work.last-work.frame);next->pts=work.path.empty()?frameTimestamp(next->frame,work.fpsNum,work.fpsDen):0;}
                    catch(const std::exception& error){next.reset();result.error=errorText(error.what());}
                }
                decoded.push_back(std::move(result));
            }
        }
    }
};
PlaybackDecoder::PlaybackDecoder(Path trace):impl_(std::make_unique<Impl>(std::move(trace))){}
PlaybackDecoder::~PlaybackDecoder()=default;
void PlaybackDecoder::shutdown(){impl_->shutdown();}
void PlaybackDecoder::invalidate(uint64_t generation){
    {std::lock_guard lock(impl_->mutex);impl_->epoch=generation;impl_->request.reset();impl_->decoded.clear();}impl_->wake.notify_one();
}
void PlaybackDecoder::submit(DecodeRequest work){
    {std::lock_guard lock(impl_->mutex);require(!impl_->closing,"Playback decoder is closed.");if(work.generation!=impl_->epoch)return;
        if(!impl_->failure.empty()){DecodeResult result;result.frame=work.frame;result.generation=work.generation;result.error=impl_->failure;impl_->decoded.push_back(std::move(result));}
        else impl_->request=std::move(work);}impl_->wake.notify_one();
}
std::optional<DecodeResult> PlaybackDecoder::takeDue(uint64_t generation,bool playing,int64_t now,uint64_t* skipped){
    std::optional<DecodeResult> result;bool consumed=false;
    {std::lock_guard lock(impl_->mutex);while(!impl_->decoded.empty()){
        auto& candidate=impl_->decoded.front();
        if(candidate.generation!=generation){impl_->decoded.pop_front();consumed=true;continue;}
        if(playing&&candidate.error.empty()&&candidate.image&&candidate.image->pts>now)break;
        if(result&&skipped)++*skipped;result=std::move(candidate);impl_->decoded.pop_front();consumed=true;
        if(!playing||!result->error.empty())break;
    }}
    if(consumed)impl_->wake.notify_one();return result;
}
}
