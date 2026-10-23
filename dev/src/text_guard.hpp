#pragma once
#include "fajk/types.hpp"
namespace fajk {
// Sparse, exact references captured before any geometric reconstruction.
// Only protected row intervals are retained, not a second full RGBA image.
struct DecodedTextProtection {
    struct Range {size_t offset=0,count=0;bool early=false;};
    uint32_t width=0,height=0;
    std::vector<Range> ranges;
    Bytes pixels;
};
DecodedTextProtection captureDecodedText(std::span<const uint8_t>,uint32_t,uint32_t,uint32_t);
// Uncertain single strokes retain their post-contour reference. Strong glyph
// clusters keep the early reference; this avoids undoing cleaned object lines.
void refreshDecodedTextFallback(DecodedTextProtection&,std::span<const uint8_t>,uint32_t,uint32_t);
void restoreDecodedText(std::span<uint8_t>,const DecodedTextProtection&,uint32_t,uint32_t);
}
