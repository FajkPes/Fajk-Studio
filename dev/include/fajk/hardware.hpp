#pragma once
#include <cstdint>
#include <functional>
#include <atomic>
namespace fajk {
struct HardwareInfo {
    uint32_t logical=1, physical=1;
    uint64_t totalMiB=0, availableMiB=0, safeMiB=0;
};
HardwareInfo hardwareInfo();
bool hasFma();
uint32_t workerLimit(uint32_t requested=0);
uint32_t safeWorkers(uint32_t width,uint32_t height,bool lossless,uint32_t requested=0,uint64_t memoryMiB=0);
// A frame validates affinity/memory once. parallelFor divides this ceiling
// among its participants, including their nested work. A new outer frame
// rechecks current system limits. Explicit smaller scopes can only reduce it.
class ScopedWorkerBudget {
    uint32_t previous_;
public:
    explicit ScopedWorkerBudget(uint32_t validated);
    ~ScopedWorkerBudget();
    ScopedWorkerBudget(const ScopedWorkerBudget&) = delete;
    ScopedWorkerBudget& operator=(const ScopedWorkerBudget&) = delete;
};
// Several frame callers may borrow currently unused callback slots. Callers
// themselves are reserved; acquisition never blocks, including nested loops.
// The owner must outlive every bound caller and its joined parallelFor jobs.
class SharedWorkerBudget {
    std::atomic<uint32_t> available_;
    friend class ParallelWorkerLease;
public:
    SharedWorkerBudget(uint32_t totalWorkers,uint32_t callers);
    SharedWorkerBudget(const SharedWorkerBudget&)=delete;
    SharedWorkerBudget& operator=(const SharedWorkerBudget&)=delete;
};
class ScopedSharedWorkerBudget {
    SharedWorkerBudget* previous_;
public:
    explicit ScopedSharedWorkerBudget(SharedWorkerBudget*);
    ~ScopedSharedWorkerBudget();
    ScopedSharedWorkerBudget(const ScopedSharedWorkerBudget&)=delete;
    ScopedSharedWorkerBudget& operator=(const ScopedSharedWorkerBudget&)=delete;
};
void parallelFor(uint32_t count,uint32_t threads,const std::function<void(uint32_t)>& work);
}
