#pragma once
#include "fajk/types.hpp"

namespace fajk {
// Exact floor(index * 10^7 * denominator / numerator), without overflowing
// intermediate products for valid large frame indices or rational rates.
inline int64_t frameTimestamp(uint64_t index,uint32_t numerator,uint32_t denominator){
    require(numerator&&denominator,"Invalid frame rate.");
    const uint64_t ticks=10000000ull*denominator;
    const uint64_t whole=index/numerator,remainder=index%numerator;
    require(whole<=uint64_t(INT64_MAX)/ticks,"Frame timestamp exceeds the supported time range.");
    const uint64_t fraction=remainder*(ticks/numerator)+remainder*(ticks%numerator)/numerator;
    const uint64_t base=whole*ticks;
    require(fraction<=uint64_t(INT64_MAX)-base,"Frame timestamp exceeds the supported time range.");
    return int64_t(base+fraction);
}
}
