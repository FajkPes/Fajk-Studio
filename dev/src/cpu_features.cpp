#include <intrin.h>
namespace fajk {
bool hasAvx2(){static const bool available=[] {int cpu[4];__cpuid(cpu,0);if(cpu[0]<7)return false;__cpuid(cpu,1);if((cpu[2]&(1<<27))==0||(cpu[2]&(1<<28))==0||(_xgetbv(0)&6)!=6)return false;__cpuidex(cpu,7,0);return (cpu[1]&(1<<5))!=0;}();return available;}
bool hasFma(){static const bool available=[] {if(!hasAvx2())return false;int cpu[4];__cpuid(cpu,1);return (cpu[2]&(1<<12))!=0;}();return available;}
}
