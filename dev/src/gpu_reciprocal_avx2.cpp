#include <immintrin.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace fajk {
// Match this host's RCPPS estimate, rather than substitute a differently rounded
// GPU reciprocal. Unsupported estimate tables simply decline the optional path.
bool nativeReciprocalTableAvx2(std::array<uint32_t,4096>& result){
    struct Table {
        std::array<uint32_t,4096> values{};bool valid=true;
        Table(){
            for(uint32_t i=0;i<values.size();++i)
                values[i]=std::bit_cast<uint32_t>(_mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(std::bit_cast<float>(0x3f800000u+(i<<11))))));
            alignas(32) uint32_t lanes[8];
            for(uint32_t base=0;base<(1u<<23)&&valid;base+=8){
                const auto input=_mm256_setr_epi32(0x3f800000+base,0x3f800001+base,0x3f800002+base,0x3f800003+base,
                                                 0x3f800004+base,0x3f800005+base,0x3f800006+base,0x3f800007+base);
                _mm256_store_si256(reinterpret_cast<__m256i*>(lanes),_mm256_castps_si256(_mm256_rcp_ps(_mm256_castsi256_ps(input))));
                for(unsigned lane=0;lane<8;++lane)valid&=lanes[lane]==values[(base+lane)>>11];
            }
            for(int exponent=-10;exponent<=28&&valid;++exponent)for(uint32_t sample=0;sample<8192;++sample){
                const auto mantissa=(sample*1051u)&0x7fffffu;
                const float own=_mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(std::ldexp(std::bit_cast<float>(0x3f800000u+mantissa),exponent))));
                const auto scaled=values[mantissa>>11]+uint32_t(-exponent*0x800000);
                valid&=std::bit_cast<uint32_t>(own)==scaled;
            }
            _mm256_zeroupper();
        }
    };
    static const Table table;
    if(table.valid)result=table.values;
    return table.valid;
}
}
