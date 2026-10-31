#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace fajk {
using SparseCoefficient=std::array<uint32_t,2>; // absolute index, unmodified float bits
// Appends at most limit entries. Caller checks AVX2 support; negative zero is
// retained too. A false result discards this optional representation.
bool appendSparseCoefficientsAvx2(std::span<const float>,uint32_t offset,size_t limit,std::vector<SparseCoefficient>&);
}
