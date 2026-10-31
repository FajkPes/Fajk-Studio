#pragma once
namespace fajk {
// Per encode-call input/output. The owner decides sequence ordering; workers
// never share mutable rate state. A preference cannot override a packet cap.
struct WaveletRateControl {float preferredStep=0,actualStep=0;bool denseHistogram=false,smoothColorModel=false;float minimumFill=0;};
inline thread_local WaveletRateControl* waveletRateControl=nullptr;
struct ScopedWaveletRateControl {
    WaveletRateControl* previous;
    explicit ScopedWaveletRateControl(WaveletRateControl* value):previous(waveletRateControl){waveletRateControl=value;}
    ~ScopedWaveletRateControl(){waveletRateControl=previous;}
};
}
