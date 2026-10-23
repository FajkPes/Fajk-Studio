#include "fajk/hardware.hpp"
#include "fajk/types.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <exception>
#include <mutex>
namespace fajk {
namespace { thread_local uint32_t frameWorkerBudget = 0; }
ScopedWorkerBudget::ScopedWorkerBudget(uint32_t validated):previous_(frameWorkerBudget){
    require(validated>0,"Missing validated worker budget.");
    frameWorkerBudget=previous_?std::min(previous_,validated):validated;
}
ScopedWorkerBudget::~ScopedWorkerBudget(){frameWorkerBudget=previous_;}
HardwareInfo hardwareInfo(){
    HardwareInfo h;
    h.logical=std::max(1ul,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    DWORD_PTR process=0,system=0;
    if(h.logical<=64&&GetProcessAffinityMask(GetCurrentProcess(),&process,&system))h.logical=std::max(1,std::popcount(process));
    // CPU topology is stable for this process; available memory and process
    // affinity are still queried on every call for the allocation safety check.
    static const uint32_t physical=[](){DWORD bytes=0;GetLogicalProcessorInformationEx(RelationProcessorCore,nullptr,&bytes);uint32_t count=0;
        if(bytes){Bytes buffer(bytes);if(GetLogicalProcessorInformationEx(RelationProcessorCore,reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),&bytes))for(DWORD i=0;i<bytes;){auto p=reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()+i);if(!p->Size)break;++count;i+=p->Size;}}
        return std::max(1u,count);
    }();h.physical=physical;
    h.physical=std::clamp(h.physical,1u,h.logical);
    MEMORYSTATUSEX m{sizeof(m)};
    if(GlobalMemoryStatusEx(&m)){
        h.totalMiB=m.ullTotalPhys/1048576;h.availableMiB=std::min(m.ullAvailPhys,m.ullAvailPageFile)/1048576;
        const auto reserve=std::max<uint64_t>(1024,h.totalMiB/8);
        h.safeMiB=h.availableMiB>reserve?std::min(h.totalMiB*3/5,(h.availableMiB-reserve)*3/4):0;
    }
    return h;
}
uint32_t workerLimit(uint32_t requested){
    if(frameWorkerBudget)return requested?std::clamp(requested,1u,frameWorkerBudget):frameWorkerBudget;
    uint32_t logical=std::max(1ul,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));DWORD_PTR process=0,system=0;
    if(logical<=64&&GetProcessAffinityMask(GetCurrentProcess(),&process,&system))logical=std::max(1,std::popcount(process));
    return requested?std::clamp(requested,1u,logical):std::max(1u,logical>2?logical-2:1u);
}
uint32_t safeWorkers(uint32_t width,uint32_t height,bool lossless,uint32_t requested,uint64_t memoryMiB){
    const auto raw=pixelBytes(width,height);const auto h=hardwareInfo();
    const uint64_t budget=(memoryMiB?std::min(memoryMiB,h.safeMiB):h.safeMiB)*1048576;
    const uint64_t pixels=uint64_t((width+63)&~63u)*((height+63)&~63u);
    // Source, displayed frames, packet staging, shared entropy, decoder surfaces.
    const uint64_t reserve=uint64_t(raw)*8+pixels*24+128*1048576ull;
    const bool tiled=uint64_t(width)*height>=512*512;
    const uint64_t job=uint64_t((width+63)&~63u)*(tiled?256:((height+63)&~63u))*(lossless?32:80)+4*1048576ull;
    require(budget>reserve&&budget-reserve>=job,"Not enough available RAM for this resolution. Close other applications or reduce the input resolution.");
    const auto wanted=requested?std::clamp(requested,1u,h.logical):std::max(1u,h.logical>2?h.logical-2:1u);
    return uint32_t(std::min<uint64_t>(wanted,(budget-reserve)/job));
}
namespace {
struct Pool {
    PTP_POOL pool=CreateThreadpool(nullptr);TP_CALLBACK_ENVIRON environment{};
    Pool(){require(pool!=nullptr,"Cannot create codec worker pool.");InitializeThreadpoolEnvironment(&environment);SetThreadpoolThreadMaximum(pool,hardwareInfo().logical);SetThreadpoolCallbackPool(&environment,pool);}
    ~Pool(){CloseThreadpool(pool);DestroyThreadpoolEnvironment(&environment);}
};
struct Batch {
    std::atomic<uint32_t> next{0};uint32_t count,chunk,budget;const std::function<void(uint32_t)>& work;
    std::mutex mutex;std::exception_ptr error;
    void run() noexcept {try{ScopedWorkerBudget scope(budget);for(;;){auto begin=next.fetch_add(chunk,std::memory_order_relaxed);if(begin>=count)return;for(uint32_t i=begin;i<std::min(count,begin+chunk);++i)work(i);}}catch(...){next.store(count);std::lock_guard lock(mutex);if(!error)error=std::current_exception();}}
};
}
void parallelFor(uint32_t count,uint32_t threads,const std::function<void(uint32_t)>& work){
    if(!count)return;
    const auto budget=workerLimit(threads),n=std::min(count,budget);
    // Each outer participant owns a disjoint share. Nested loops may use that
    // share, never multiply the full frame budget by the number of parents.
    // With only one task the entire requested budget remains available to it.
    const auto share=std::max(1u,budget/n);
    if(n<=1){ScopedWorkerBudget scope(share);for(uint32_t i=0;i<count;++i)work(i);return;}
    static Pool pool;Batch batch{{0},count,std::clamp(count/(n*8),1u,4u),share,work,{},nullptr};
    auto task=CreateThreadpoolWork([](PTP_CALLBACK_INSTANCE,void* context,PTP_WORK){static_cast<Batch*>(context)->run();},&batch,&pool.environment);
    require(task!=nullptr,"Cannot schedule codec workers.");
    for(uint32_t i=1;i<n;++i)SubmitThreadpoolWork(task);batch.run();
    // The caller has claimed every job (or recorded a failure). Pending
    // callbacks can only find an empty queue: cancel those, but still join
    // callbacks already running. This also avoids waiting for empty nested
    // callbacks behind other callers in a busy shared pool.
    WaitForThreadpoolWorkCallbacks(task,TRUE);CloseThreadpoolWork(task);
    if(batch.error)std::rethrow_exception(batch.error);
}
}
