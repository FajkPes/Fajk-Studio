#pragma once
#include <cstdint>
#include <functional>
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
void parallelFor(uint32_t count,uint32_t threads,const std::function<void(uint32_t)>& work);
}
