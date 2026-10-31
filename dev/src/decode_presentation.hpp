#pragma once
#include <cstdint>
namespace fajk {
// Borrowed for one final reconstruction. A successful resident GPU path may
// consume sharpening; callers otherwise apply the ordinary final CPU stage.
struct DecodePresentation {
    uint32_t sharpening=0;
    bool applied=false;
};
}
