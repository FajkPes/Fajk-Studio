#include "../debug_inspection.hpp"
#include "../reconstruction441.hpp"
#include "../frame_time.hpp"
#include "../frame_pool.hpp"
#include "../playback_decoder.hpp"
#include "../cleanup_quality.hpp"
#include "../output_safety.hpp"
#include "fajk/codec.hpp"
#include "fajk/compute.hpp"
#include "../decode_internal.hpp"
#include "fajk/encoder_catalog.hpp"
#include "fajk/container.hpp"
#include "fajk/media.hpp"
#include "fajk/pipeline.hpp"
#include "fajk/hardware.hpp"
#include "fajk/pixels.hpp"
#include <windows.h>
#undef GetCurrentTime
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <robuffer.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <mutex>
#include <optional>
#include <tuple>
#include <map>
#include <sstream>
#include <thread>
#include <deque>
#include <cwctype>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Foundation;
using namespace fajk;
namespace {
std::shared_ptr<fajk::Frame> decodeStudio(FrameCodec& codec, const Packet& packet, uint32_t width, uint32_t height,
                                          uint32_t workers, uint32_t deblocking, uint32_t sharpening) {
    static FramePool frames;
    auto result = frames.acquire(width, height, packet.pts, packet.duration);
    codec.setThreads(workers);codec.setDeblocking(deblocking);codec.setSharpening(sharpening);
    codec.decodeInto(packet,width,height,result->pixels);
    return result;
}
bool samePreviewCompression(const EncodeOptions& a, const EncodeOptions& b) {
    return a.shapeIntensity == b.shapeIntensity && a.contourIntensity == b.contourIntensity &&
           a.adaptiveQuantization == b.adaptiveQuantization &&
           a.adaptiveQuantizationEnabled == b.adaptiveQuantizationEnabled &&
           a.adaptiveAllocationStrength == b.adaptiveAllocationStrength && a.flatSavings == b.flatSavings &&
           a.flatThreshold == b.flatThreshold && a.flatAllocation == b.flatAllocation && a.colorEdgePriority == b.colorEdgePriority &&
           a.revision == b.revision && a.backend == b.backend && a.speed == b.speed && a.quality == b.quality && a.chroma420 == b.chroma420 &&
           a.chroma422 == b.chroma422 && a.lossless == b.lossless && a.targetBytes == b.targetBytes &&
           a.automaticTarget == b.automaticTarget;
}
Path executableDirectory() {
    wchar_t path[32768];
    GetModuleFileNameW(nullptr, path, 32768);
    return Path(path).parent_path();
}
std::wstring number(double v, int decimals = 1) {
    std::wostringstream s;
    s << std::fixed << std::setprecision(decimals) << v;
    return s.str();
}
std::wstring bytes(uint64_t v) {
    return v < 1000000 ? number(v / 1000.) + L" kB" : number(v / 1000000.) + L" MB";
}
bool fajkFile(Path const& path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return ext == L".fajk" || ext == L".fajkv";
}
bool usesBlockFilter(Packet const&) {
    return false;
}
bool usesColorFilter(Packet const&) {
    return false;
}
bool usesSharpFilter(Packet const& packet) {
    return packet.data.size()>=5;
}
struct Result {
    enum Kind { Source, Sequence, Preview, Converted, Exported, Linked, Debug } kind = Source;
    std::shared_ptr<DebugInspection> debug;
    uint64_t debugEpoch = 0;
    std::shared_ptr<Packet> previewPacket;
    std::optional<EncodeOptions> encodedOptions;
    Path path;
    MediaInfo media{};
    SequenceInfo sequence{};
    std::vector<IndexEntry> index;
    std::shared_ptr<fajk::Frame> frame;
    uint64_t size = 0, sourceBytes = 0;
    double encodeMs = -1, decodeMs = 0;
    bool legacyFilter = false, colorFilter = false, sharpFilter = false, reusedCompression = false;
    std::wstring error, allocationSavings;
};
struct Studio : ApplicationT<Studio, Markup::IXamlMetadataProvider> {
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
    Window window{nullptr};
    FrameworkElement root{nullptr};
    DispatcherTimer timer{nullptr};
    HWND hwnd{};
    MediaRuntime mediaRuntime;
    Path sourcePath, sequencePath;
    MediaInfo sourceInfo{};
    SequenceInfo sequenceInfo{};
    std::vector<IndexEntry> index;
    uint64_t originalFileBytes = 0;
    std::shared_ptr<fajk::Frame> original, shown, compressed;
    std::shared_ptr<Packet> previewPacket;
    std::optional<EncodeOptions> previewOptions;
    std::weak_ptr<fajk::Frame> previewOriginal;
    ConvertOptions options;
    ComputeBackend decoderBackend=ComputeBackend::Cpu;
    uint32_t deblocking = 100, sharpening = 50;
    bool legacyFilter = false, colorFilter = false, sharpFilter = false;
    uint32_t presentedFilter = 100, presentedSharpening = 50;
    winrt::Windows::Media::Playback::MediaPlayer audio{nullptr};
    winrt::Windows::Media::Playback::MediaPlaybackItem audioItem{nullptr};
    std::atomic<bool> audioOpened = false, audioFailed = false, audioEnded = false;
    std::shared_ptr<BudgetHeatmap> budgetMap, smokeBudget;
    std::wstring allocationSavings;
    uint32_t smokeFlatAllocation = 50;
    uint32_t smokeFlatSavings = 50, smokeFlatThreshold = 0;
    bool debugMode = false, debugPending = false, debugInFlight = false, debugEncodePending = false;
    std::chrono::steady_clock::time_point debugChanged;
    std::jthread debugWorker;
    std::optional<Result> debugResult;
    std::shared_ptr<DebugInspectionCache> debugCache;
    std::array<std::string, debugViewNames.size()> debugCosts;
    int debugWheelRemainder[2]{};
    uint64_t debugEpoch = 0;
    std::shared_ptr<fajk::Frame> debugFrame;
    const uint8_t* smokeDebugCachedData = nullptr;
    WriteableBitmap debugBitmap{nullptr};
    std::weak_ptr<fajk::Frame> debugUploaded;
    bool sidebarVisible = true;
    std::map<std::wstring, uint64_t> settingsWritten;
    bool retiredSettingsRemoved = false;
    bool hasAudio = false, fitMode = true, settingMode = false, dragging = false, dragSplit = false;
    double dragX = 0, dragY = 0, offsetX = 0, offsetY = 0, viewScale = 1.;
    WriteableBitmap originalBitmap{nullptr}, compressedBitmap{nullptr}, budgetBitmap{nullptr};
    std::shared_ptr<fajk::Frame> budgetFrame;
    std::weak_ptr<fajk::Frame> budgetUploaded;
    std::weak_ptr<fajk::Frame> originalUploaded, compressedUploaded;
    uint64_t audioGeneration = 0;
    int renderedMode = -1;
    uint32_t renderedWidth = 0, renderedHeight = 0;
    bool renderedOriginal = false, renderedCompressed = false;

    std::jthread job;
    std::unique_ptr<PlaybackDecoder> decoder;
    std::mutex mutex;
    std::condition_variable jobWake;
    std::function<Result(std::stop_token)> pendingJob;
    std::stop_source jobStop;
    bool jobClosing = false;
    std::optional<Result> result;
    std::optional<Progress> progress;
    winrt::event_token renderingToken{};
    bool filterPending = false, filterJob = false;
    std::chrono::steady_clock::time_point filterChanged;
    bool busy = false, closing = false, playing = false, settingTimeline = false,
         fullscreen = false;
    uint64_t current = 0, requested = UINT64_MAX, generation = 0, contentRevision = 0;
    int64_t clockBase = 0;
    std::chrono::steady_clock::time_point clockStart;
    uint32_t smokeDeblocking = 100, smokeTargetFilter = 0;
    std::shared_ptr<Packet> smokePacket;
    std::shared_ptr<fajk::Frame> smokeFiltered;
    uint32_t smokeSharpening = 50, smokeTargetSharp = 0;
    uint32_t smokeSurfaces = 100, smokeContours = 100;
    Reconstruction441 smokeR441;
    uint32_t smokeR441Intensity=50;
    Path initial;
    bool smokeGpu=false,smokeFast=false;
    bool smokeSpeedChecked=false;
    int smokeSavedSpeed=0;
    std::shared_ptr<Packet> smokeSpeedPacket;
    std::shared_ptr<fajk::Frame> smokeSpeedFrame;
    std::optional<GpuInfo> computeDevice;
    bool smoke = false, smokeHeatmap = false, smokeDebug = false;
    int smokeStage = 0;
    uint64_t smokePresented = 0, smokeSkipped = 0;
    double smokeZoomX = 0, smokeZoomY = 0;
    winrt::Windows::Foundation::Point smokeZoomAnchor{};
    std::chrono::steady_clock::time_point smokeStart;
    double smokeUploadMs = 0;
    std::map<std::wstring, winrt::Windows::Foundation::IInspectable> named;

    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& name) {
        return metadata.GetXamlType(name);
    }
    Markup::IXamlType GetXamlType(hstring const& name) { return metadata.GetXamlType(name); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return metadata.GetXmlnsDefinitions(); }
    template <class T> T find(const wchar_t* name) {
        auto it = named.find(name);
        if (it == named.end()) {
            auto value = root.FindName(name);
            if (!value)
                throw std::runtime_error("Missing named XAML control: " + to_string(name));
            it = named.emplace(name, value).first;
        }
        return it->second.as<T>();
    }
    void status(std::wstring const& text) { find<TextBlock>(L"Status").Text(text); }
    void showSizeChange(uint64_t before, uint64_t after, std::wstring const& basis) {
        auto panel = find<Border>(L"SizeChangePanel");
        panel.Visibility(before ? Visibility::Visible : Visibility::Collapsed);
        if (!before)
            return;
        const double percent = 100. * (double(before) - double(after)) / double(before);
        const bool smaller = after<before, larger = after> before;
        const std::wstring change = smaller  ? L"−" + number(percent, 2) + L" % · menší"
                                    : larger ? L"+" + number(-percent, 2) + L" % · větší"
                                             : L"0 % · stejná velikost";
        auto text = find<TextBlock>(L"SizeChange");
        text.Text(bytes(before) + L" → " + bytes(after) + L"\n" + change);
        const bool dark = root.ActualTheme() == ElementTheme::Dark;
        text.Foreground(SolidColorBrush(
            smaller
                ? (dark ? winrt::Windows::UI::Color{255, 108, 203, 143} : winrt::Windows::UI::Color{255, 16, 124, 65})
            : larger
                ? (dark ? winrt::Windows::UI::Color{255, 255, 153, 164} : winrt::Windows::UI::Color{255, 196, 43, 28})
                : (dark ? winrt::Windows::UI::Color{255, 190, 190, 190} : winrt::Windows::UI::Color{255, 96, 96, 96})));
        find<TextBlock>(L"SizeChangeBasis").Text(basis);
        Automation::AutomationProperties::SetName(panel, basis + L": " + text.Text().c_str());
    }
    void error(std::wstring const& text) {
        auto bar = find<InfoBar>(L"Error");
        bar.Message(text);
        bar.IsOpen(!text.empty());
    }
    void bind(const wchar_t* name, std::function<void()> action) {
        find<Button>(name).Click([this, action](auto&&, auto&&) {
            try {
                action();
            } catch (hresult_error const& e) {
                error(e.message().c_str());
            } catch (std::exception const& e) {
                error(to_hstring(e.what()).c_str());
            }
        });
    }
    uint32_t every(const wchar_t* name) { return 1u << std::max(0, find<ComboBox>(name).SelectedIndex()); }
    double speed() {
        static constexpr double values[]{.5, 1, 2, 4};
        return values[std::clamp(find<ComboBox>(L"Speed").SelectedIndex(), 0, 3)];
    }
    EncodeOptions encodeOptions() {
        auto value = options.encode;
        value.speed=EncodeSpeed(std::clamp(find<ComboBox>(L"EncodeSpeed").SelectedIndex(),0,2));
        value.quality = float(find<Slider>(L"Quality").Value());
        value.shapeIntensity = uint32_t(find<Slider>(L"ShapeIntensity").Value());
        value.contourIntensity = uint32_t(find<Slider>(L"ContourIntensity").Value());
        value.flatAllocation = uint32_t(find<Slider>(L"FlatAllocation").Value());
        value.flatSavings = uint32_t(find<Slider>(L"FlatSavings").Value());
        value.flatThreshold = uint32_t(find<Slider>(L"FlatThreshold").Value());
        value.adaptiveQuantizationEnabled = find<CheckBox>(L"AdaptiveQuantizationEnabled").IsChecked().Value();
        const auto selected = find<ComboBox>(L"CodecVersion").SelectedIndex();
        value.revision = selected >= 0 && size_t(selected) < encoderCatalog.size() ? encoderCatalog[selected].revision
                                                                                   : defaultEncoderRevision;
        value.backend=selected>=0&&size_t(selected)<encoderCatalog.size()?encoderCatalog[selected].backend:ComputeBackend::Cpu;
        value.chroma420 = find<ComboBox>(L"ChromaMode").SelectedIndex() == 0;
        value.chroma422 = find<ComboBox>(L"ChromaMode").SelectedIndex() == 2;
        value.threads = options.workers;
        value.lossless = false;
        const bool automaticSkirt=find<CheckBox>(L"R441Automatic").IsChecked().Value();
        value.reconstruction441.intensity = skirtQualityIntensity(uint32_t(find<Slider>(L"R441Intensity").Value()),value.quality,automaticSkirt);
        value.reconstruction441.fixedIntensity = !automaticSkirt;
        value.reconstruction441.shapes.tolerance=uint32_t(find<Slider>(L"ShapeTolerance").Value());
        value.reconstruction441.shapes.minimumSize=uint32_t(find<Slider>(L"ShapeMinimumSize").Value());
        value.reconstruction441.strongerTangents = uint32_t(find<Slider>(L"StrongerTangents").Value());
        value.reconstruction441.antialias = uint32_t(find<Slider>(L"R441Antialias").Value());
        value.reconstruction441.contrast = uint32_t(find<Slider>(L"R441Contrast").Value());
        value.reconstruction441.style = uint32_t(std::max(0, find<ComboBox>(L"R441Style").SelectedIndex()));
        value.reconstruction441.protectText = find<CheckBox>(L"R441ProtectText").IsChecked().Value();
        const int budget = find<ComboBox>(L"Budget").SelectedIndex();
        static constexpr uint32_t sizes[]{0, 27000, 30000, 50000, 60000, 80000, 120000, 150000, 200000, 0};
        value.targetBytes = sizes[std::clamp(budget, 0, 9)];
        if (sourceInfo.image && value.targetBytes)
            value.targetBytes -= 168;
        value.automaticTarget = budget == 0;
        return value;
    }
    bool sourceVideo() const { return !sourcePath.empty() && !sourceInfo.image; }
    int64_t duration() const {
        return !index.empty() ? sequenceInfo.duration : sourceVideo() ? sourceInfo.duration : 0;
    }
    bool playable() const { return duration() > 0 && (!index.empty() ? index.size() > 1 : sourceVideo()); }
    int64_t position() const { return shown ? shown->pts : 0; }
    uint64_t lastFrame() const {
        return !index.empty() ? index.size() - 1
               : sourceVideo()
                   ? uint64_t(
                         std::max(0., std::ceil(sourceInfo.duration / 1e7 * sourceInfo.fpsNum / sourceInfo.fpsDen) - 1))
                   : 0;
    }
    int64_t frameTime(uint64_t i) const {
        return !index.empty() ? index[std::min<uint64_t>(i, index.size() - 1)].pts
                              : frameTimestamp(i, sourceInfo.fpsNum, sourceInfo.fpsDen);
    }
    void updateEnabled() {
        for (auto name : {L"Open", L"OpenSequence", L"Settings", L"CodecVersion", L"EncodeSpeed", L"ChromaMode", L"ContourIntensity",
                          L"ShapeIntensity", L"R441Intensity", L"StrongerTangents", L"R441Antialias", L"R441Contrast",
                          L"R441Style", L"R441ProtectText", L"R441Automatic", L"ShapeTolerance", L"ShapeMinimumSize", L"ResetShapeTuning", L"FlatAllocation", L"FlatSavings", L"FlatThreshold", L"AdaptiveQuantizationEnabled", L"HeatmapToggle", L"Attach"})
            find<Control>(name).IsEnabled(!busy);
        find<Slider>(L"FlatAllocation").IsEnabled(!busy &&
            find<CheckBox>(L"AdaptiveQuantizationEnabled").IsChecked().Value());
        find<Slider>(L"FlatThreshold").IsEnabled(!busy && find<CheckBox>(L"AdaptiveQuantizationEnabled").IsChecked().Value());
        find<Slider>(L"FlatSavings").IsEnabled(!busy &&
            find<CheckBox>(L"AdaptiveQuantizationEnabled").IsChecked().Value());
        find<ComboBox>(L"Budget").IsEnabled(!busy);
        find<Expander>(L"VideoControls").Visibility(sourceVideo() ? Visibility::Visible : Visibility::Collapsed);
        find<ComboBox>(L"Every").Visibility(sourceVideo() ? Visibility::Visible : Visibility::Collapsed);
        find<ComboBox>(L"Every").IsEnabled(!busy && sourceVideo());
        find<ToggleSwitch>(L"Temporal").IsEnabled(!busy&&sourceVideo());
        find<ComboBox>(L"TemporalMethod").IsEnabled(!busy&&sourceVideo()&&find<ToggleSwitch>(L"Temporal").IsOn());
        find<NumberBox>(L"KeyframeInterval").IsEnabled(!busy&&sourceVideo()&&find<ToggleSwitch>(L"Temporal").IsOn());
        const auto temporalMethod=find<ComboBox>(L"TemporalMethod").SelectedIndex();
        const bool temporalEnabled=!busy&&sourceVideo()&&find<ToggleSwitch>(L"Temporal").IsOn();
        for(const auto name:{L"SimilarTolerance",L"SimilarHold"}){
            find<NumberBox>(name).IsEnabled(temporalEnabled&&temporalMethod==1);
            find<NumberBox>(name).Visibility(temporalMethod==1?Visibility::Visible:Visibility::Collapsed);
        }
        find<NumberBox>(L"MaxTolerance").IsEnabled(temporalEnabled&&temporalMethod==2);
        find<NumberBox>(L"MaxTolerance").Visibility(temporalMethod==2?Visibility::Visible:Visibility::Collapsed);
        find<Expander>(L"LegacyFilter").Visibility(sharpFilter?Visibility::Visible:Visibility::Collapsed);
        find<Slider>(L"Deblocking").IsEnabled((!busy || filterJob) && (legacyFilter || colorFilter));
        find<Expander>(L"LegacyFilter").Header(box_value(L"Finální ostření · 0–200 %"));
        find<StackPanel>(L"SharpnessPanel").Visibility(sharpFilter ? Visibility::Visible : Visibility::Collapsed);
        find<StackPanel>(L"SmoothingPanel").Visibility(Visibility::Collapsed);
        find<Slider>(L"Sharpening").IsEnabled(sharpFilter && (!busy || filterJob));
        find<TextBlock>(L"FilterName")
            .Text(colorFilter ? L"Vyhlazení barev a jemných vad" : L"Deblocking · vyhlazení hran bloků");
        find<TextBlock>(L"FilterHint")
            .Text(colorFilter ? L"0 % vypne vyhlazení barev; podporované profily tlumí i jemné lemy. Změna upraví "
                                L"náhled a export PNG bez nové komprese."
                  : legacyFilter
                      ? L"0 % vypne deblocking. Vyhlazuje slabé přechody mezi bloky a chrání silné hrany. Změna upraví "
                        L"náhled a export PNG bez nové komprese."
                      : L"Filtr bude dostupný po výpočtu ztrátového náhledu, pokud jej použitý profil podporuje.");
        find<Slider>(L"Quality").IsEnabled(!busy);
        find<Button>(L"Convert").IsEnabled(!busy && !sourcePath.empty());
        find<Button>(L"Preview").IsEnabled(!busy && bool(original) && sequencePath.empty());
        find<Button>(L"Cancel").Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        for (auto name : {L"Play", L"Next", L"Previous", L"Timeline", L"Step", L"Speed"})
            find<Control>(name).IsEnabled(!busy && playable());
        for (auto name : {L"Timeline", L"PlaybackControls", L"Step", L"Speed", L"AudioState"})
            find<UIElement>(name).Visibility(playable() ? Visibility::Visible : Visibility::Collapsed);
        for (auto name : {L"Export", L"Zoom", L"Fit", L"ZoomIn", L"ZoomOut", L"Fullscreen"})
            find<Control>(name).IsEnabled(!busy && bool(shown));
        if (debugMode) {
            for (auto name : {L"DebugLeft", L"DebugRight", L"DebugGain", L"DebugNormal", L"DebugClose"})
                find<Control>(name).IsEnabled(true);
            for (auto name : {L"DebugInspect", L"CaptureBudgetMaps", L"HeatmapKind", L"DebugBudget"})
                find<Control>(name).IsEnabled(!busy);
        }
        auto modes = find<ComboBox>(L"ViewMode");
        modes.IsEnabled(!busy && bool(shown) && !(debugMode && debugFrame));
        modes.Items().GetAt(0).as<ComboBoxItem>().IsEnabled(!sourcePath.empty());
        modes.Items().GetAt(1).as<ComboBoxItem>().IsEnabled(bool(compressed) || !sequencePath.empty());
        modes.Items().GetAt(2).as<ComboBoxItem>().IsEnabled(!sourcePath.empty() &&
                                                            (bool(compressed) || !sequencePath.empty()));
        find<Button>(L"Attach").Visibility(sequencePath.empty() || !sourcePath.empty() ? Visibility::Collapsed
                                                                                       : Visibility::Visible);
        find<Slider>(L"Volume").IsEnabled(hasAudio);
        find<Button>(L"Mute").IsEnabled(hasAudio);
    }
    void bitmap(std::shared_ptr<fajk::Frame> const& frame, WriteableBitmap& image,
                std::weak_ptr<fajk::Frame>& uploaded) {
        if (uploaded.lock() == frame)
            return;
        if (!image || image.PixelWidth() != int(frame->width) || image.PixelHeight() != int(frame->height))
            image = WriteableBitmap(int(frame->width), int(frame->height));
        uint8_t* output = nullptr;
        check_hresult(image.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&output));
        displayPixels(frame->pixels, std::span(output, frame->pixels.size()));
        image.Invalidate();
        uploaded = frame;
    }
    void setMode(int mode) {
        settingMode = true;
        find<ComboBox>(L"ViewMode").SelectedIndex(mode);
        settingMode = false;
        render();
    }
    void requestDebug() {
        if (!debugMode)
            return;
        ++debugEpoch; // Reject a completed view if the user has already selected another.
        debugPending = true;
        debugChanged = std::chrono::steady_clock::now();
    }
    void invalidateDebug() {
        if (!debugMode && !debugFrame)
            return;
        requestDebug();
        ++debugEpoch;
        debugFrame.reset();
        debugCosts = {};
        debugBitmap = nullptr;
        debugUploaded.reset();
        renderedMode = -1;
        find<TextBlock>(L"DebugCaption").Text(L"Načítání aktuálních pohledů…");
        find<TextBox>(L"DebugReport").Text(L"Analýza se připravuje automaticky.");
    }
    void debugTab(bool enabled) {
        stopPlay();
        debugMode = enabled;
        find<UIElement>(L"CompressionControls").Visibility(Visibility::Visible);
        find<Button>(L"DebugTab").Content(box_value(enabled ? L"Debug mód · zapnuto" : L"Debug mód"));
        renderedMode = -1;
        if (!enabled) {
            debugCache.reset();
            find<UIElement>(L"DebugViewLegend").Visibility(Visibility::Collapsed);
            find<UIElement>(L"DebugPixelProbe").Visibility(Visibility::Collapsed);
            find<ToggleSwitch>(L"HeatmapToggle").IsOn(false);
            invalidateDebug();
            budgetFrame.reset();
            budgetMap.reset();
            allocationSavings.clear();
            find<TextBlock>(L"FlatSavingsReport").Text(L"Srovnání úspory se počítá jen v Debugu ze zdrojového obrázku.");
            debugPending = false;
            debugEncodePending = false;
            budgetBitmap = nullptr;
            budgetUploaded.reset();
            if (previewOptions)
                previewOptions->budgetHeatmap.reset();
        }
        layout();
        render();
        updateEnabled();
        if (enabled)
            requestDebug();
    }
    void rebuildBudgetMap() {
        budgetFrame.reset();
        budgetUploaded.reset();
        budgetBitmap = nullptr;
        if (!budgetMap || budgetMap->bits.empty()) {
            find<TextBlock>(L"HeatmapLegend")
                .Text(L"Mapa vyžaduje nový náhled ze zdroje. Samotný soubor neobsahuje rozhodování enkodéru.");
            return;
        }
        std::string legend;
        const int view = 16 + std::clamp(find<ComboBox>(L"HeatmapKind").SelectedIndex(), 0, 4);
        budgetFrame = std::make_shared<fajk::Frame>(budgetDebugImage(*budgetMap, view, legend));
        find<TextBlock>(L"HeatmapLegend").Text(to_hstring(legend));
        const auto probe = L"Přejeďte kurzorem nad aktuální mapou pro bity, bpp, váhy a zařazení ploch.";
        find<TextBlock>(L"DebugProbe").Text(probe);
        find<TextBlock>(L"DebugPixelProbe").Text(probe);
    }
    void captureBudgetMaps() {
        if (!debugMode || busy || !original || !sequencePath.empty()) {
            status(L"Mapy rozhodování vyžadují zdrojový obrázek a nový encode náhledu.");
            return;
        }
        if (!find<ToggleSwitch>(L"HeatmapToggle").IsOn()) {
            find<ToggleSwitch>(L"HeatmapToggle").IsOn(true);
            if (!busy)
                preview();
        } else
            preview();
    }
    void inspectDebug() {
        if (!debugMode)
            return;
        if (busy || debugInFlight) {
            requestDebug();
            return;
        }
        debugPending = false;
        if (debugEncodePending && original && sequencePath.empty()) {
            debugEncodePending = false;
            captureBudgetMaps();
            return;
        }
        debugEncodePending = false;
        stopPlay();
        invalidateDecode();
        auto packet = previewPacket;
        const auto path = sequencePath;
        const auto frameIndex = current;
        uint32_t w = compressed ? compressed->width : sequenceInfo.width,
                 h = compressed ? compressed->height : sequenceInfo.height;
        if (path.empty() && !packet) {
            if (original)
                preview();
            else
                status(L"Otevřete obrázek nebo komprimovaný soubor pro debug pohledy.");
            return;
        }
        const std::array<int, 2> views{find<ComboBox>(L"DebugLeft").SelectedIndex(),
                                       find<ComboBox>(L"DebugRight").SelectedIndex()};
        const bool needsMap = views[0] >= 16 || views[1] >= 16;
        if (needsMap && (!budgetMap || !previewOptions || !samePreviewCompression(*previewOptions, encodeOptions())) &&
            sequencePath.empty() && original) {
            captureBudgetMaps();
            return;
        }
        const auto map = budgetMap && !budgetMap->bits.empty() ? budgetMap : std::shared_ptr<BudgetHeatmap>{};
        const float gain = float(find<Slider>(L"DebugGain").Value());
        const auto epoch = ++debugEpoch;
        if(!debugCache)debugCache=std::make_shared<DebugInspectionCache>();
        const auto inspectionCache=debugCache;
        status(L"Samostatná debug analýza… běžný encode/decode se neměří.");
        debugInFlight = true;
        // Independent diagnostic worker: normal controls and codec jobs remain available.
        debugWorker = std::jthread([this, packet, path, frameIndex, w, h, views, gain, epoch, map, inspectionCache](auto stop) {
            Result r;
            r.kind = Result::Debug;
            r.debugEpoch = epoch;
            try {
                if (!stop.stop_requested()) {
                    Packet selected;
                    if (!path.empty()) {
                        SequenceReader reader(path);
                        selected = reader.read(frameIndex);
                    } else
                        selected = *packet;
                    r.debug =
                        std::make_shared<DebugInspection>(inspectPacket(selected, w, h, views, gain, 0, map.get(), inspectionCache.get()));
                }
            } catch (std::exception const& e) {
                r.error = to_hstring(e.what()).c_str();
            } catch (...) {
                r.error = L"Debug analýza se nezdařila.";
            }
            std::lock_guard lock(mutex);
            debugResult = std::move(r);
        });
    }
    void render() {
        if (debugMode && debugFrame) {
            shown = debugFrame;
            find<UIElement>(L"DebugViewLegend").Visibility(Visibility::Visible);
            find<UIElement>(L"DebugPixelProbe").Visibility(budgetMap ? Visibility::Visible : Visibility::Collapsed);
            bitmap(debugFrame, debugBitmap, debugUploaded);
            find<Image>(L"Picture").Source(debugBitmap);
            for (auto name :
                 {L"BudgetOverlay", L"Reference", L"Divider", L"CompareLeft", L"CompareRight", L"CompareControls"})
                find<UIElement>(name).Visibility(Visibility::Collapsed);
            find<StackPanel>(L"Empty").Visibility(Visibility::Collapsed);
            find<TextBlock>(L"ViewState").Text(L"DEBUG · dva pohledy · kolečko: zoom v místě kurzoru · tažení: posun");
            if (renderedMode != -2) {
                renderedMode = -2;
                resizePicture();
                if (fitMode)
                    fit();
            }
            updateEnabled();
            return;
        }

        if (debugMode) {
            find<UIElement>(L"DebugViewLegend")
                .Visibility(budgetMap && find<ToggleSwitch>(L"HeatmapToggle").IsOn() ? Visibility::Visible
                                                                                     : Visibility::Collapsed);
            find<UIElement>(L"DebugPixelProbe").Visibility(budgetMap ? Visibility::Visible : Visibility::Collapsed);
            if (budgetMap)
                find<TextBlock>(L"DebugViewLegend").Text(find<TextBlock>(L"HeatmapLegend").Text());
        }
        int mode = find<ComboBox>(L"ViewMode").SelectedIndex();
        if (mode == 2 && (!original || !compressed))
            mode = compressed ? 1 : 0;
        shown = mode == 0 ? original : compressed;
        if (!shown)
            return;
        if (debugMode && budgetFrame && find<ToggleSwitch>(L"HeatmapToggle").IsOn())
            bitmap(budgetFrame, budgetBitmap, budgetUploaded);
        find<Image>(L"BudgetOverlay").Source(budgetBitmap);
        find<Image>(L"BudgetOverlay").Opacity(find<Slider>(L"HeatmapOpacity").Value() / 100.);
        find<Image>(L"BudgetOverlay")
            .Visibility(debugMode && mode != 0 && budgetFrame && find<ToggleSwitch>(L"HeatmapToggle").IsOn()
                            ? Visibility::Visible
                            : Visibility::Collapsed);
        if (original && (mode == 0 || mode == 2))
            bitmap(original, originalBitmap, originalUploaded);
        if (compressed && mode != 0)
            bitmap(compressed, compressedBitmap, compressedUploaded);
        auto picture = find<Image>(L"Picture");
        auto activeBitmap = mode == 0 ? originalBitmap : compressedBitmap;
        if (picture.Source() != activeBitmap)
            picture.Source(activeBitmap);
        if (mode == renderedMode && shown->width == renderedWidth && shown->height == renderedHeight &&
            bool(original) == renderedOriginal && bool(compressed) == renderedCompressed) {
            updateTime();
            return;
        }
        renderedMode = mode;
        renderedWidth = shown->width;
        renderedHeight = shown->height;
        renderedOriginal = bool(original);
        renderedCompressed = bool(compressed);
        find<Image>(L"Reference").Source(originalBitmap);
        const auto visibility = mode == 2 ? Visibility::Visible : Visibility::Collapsed;
        for (auto name : {L"Reference", L"Divider", L"CompareLeft", L"CompareRight", L"CompareControls"})
            find<UIElement>(name).Visibility(visibility);
        find<TextBlock>(L"ViewState")
            .Text(mode == 2              ? L"Originál vlevo · komprimovaný vpravo · přetáhněte čáru"
                  : mode == 0            ? L"ORIGINÁL · zdrojový obraz"
                  : sequencePath.empty() ? L"KOMPRIMOVANÝ · náhled aktuálního snímku"
                                         : L"KOMPRIMOVANÝ · uložená sekvence");
        find<StackPanel>(L"Empty").Visibility(Visibility::Collapsed);
        const double dpi = root.XamlRoot() ? root.XamlRoot().RasterizationScale() : 1;
        auto surface = find<Grid>(L"Surface");
        surface.Width(shown->width / dpi * viewScale);
        surface.Height(shown->height / dpi * viewScale);
        split();
        if (fitMode)
            fit();
        updateEnabled();
        updateTime();
    }
    void split() {
        auto surface = find<Grid>(L"Surface");
        if (!shown)
            return;
        const double x = surface.Width() * find<Slider>(L"Split").Value() / 100;
        RectangleGeometry clip;
        clip.Rect({0, 0, float(x), float(surface.Height())});
        find<Image>(L"Reference").Clip(clip);
        find<Border>(L"Divider").Margin({x, 0, 0, 0});
    }
    void resizePicture() {
        if (!shown)
            return;
        const double dpi = root.XamlRoot() ? root.XamlRoot().RasterizationScale() : 1;
        auto surface = find<Grid>(L"Surface");
        surface.Width(shown->width / dpi * viewScale);
        surface.Height(shown->height / dpi * viewScale);
        split();
        find<Button>(L"Zoom").Content(box_value(std::to_wstring(int(std::round(viewScale * 100))) + L" %"));
    }
    void zoomAt(double value, winrt::Windows::Foundation::Point anchor) {
        if (!shown)
            return;
        fitMode = false;
        auto view = find<ScrollViewer>(L"Viewport");
        auto surface = find<Grid>(L"Surface");
        // Transform the pointer into image coordinates; account for centered letterboxing.
        const auto origin = surface.TransformToVisual(view).TransformPoint({0, 0});
        const double x = (anchor.X - origin.X) / viewScale, y = (anchor.Y - origin.Y) / viewScale;
        viewScale = std::clamp(value, .01, 10.);
        resizePicture();
        view.UpdateLayout();
        const double marginX = std::max(0., (view.ViewportWidth() - surface.Width()) / 2),
                     marginY = std::max(0., (view.ViewportHeight() - surface.Height()) / 2);
        view.ChangeView(std::clamp(x * viewScale + marginX - anchor.X, 0., view.ScrollableWidth()),
                        std::clamp(y * viewScale + marginY - anchor.Y, 0., view.ScrollableHeight()), nullptr, true);
    }
    void zoom(double value) {
        auto view = find<ScrollViewer>(L"Viewport");
        zoomAt(value, {float(view.ViewportWidth() / 2), float(view.ViewportHeight() / 2)});
    }
    void fit() {
        if (!shown || !fitMode)
            return;
        auto view = find<ScrollViewer>(L"Viewport");
        const double dpi = root.XamlRoot() ? root.XamlRoot().RasterizationScale() : 1;
        viewScale = std::clamp(std::min(std::max(1., view.ActualWidth() - 4) * dpi / shown->width,
                                        std::max(1., view.ActualHeight() - 4) * dpi / shown->height),
                               .01, 10.);
        resizePicture();
        view.ChangeView(0., 0., nullptr, true);
    }
    void updateTime() {
        auto stamp = [](int64_t ticks) {
            int64_t seconds = std::max<int64_t>(0, ticks / 10000000);
            return std::to_wstring(seconds / 60) + L":" + (seconds % 60 < 10 ? L"0" : L"") +
                   std::to_wstring(seconds % 60);
        };
        find<TextBlock>(L"Time").Text(stamp(position()) + L" / " + stamp(duration()));
    }
    void layout() {
        const bool pane = !fullscreen && sidebarVisible;
        find<UIElement>(L"Sidebar").Visibility(pane ? Visibility::Visible : Visibility::Collapsed);
        for (auto name : {L"Header", L"Footer"})
            find<UIElement>(name).Visibility(fullscreen ? Visibility::Collapsed : Visibility::Visible);
        auto workspace = find<Grid>(L"Workspace");
        workspace.Margin(fullscreen ? Thickness{0} : Thickness{20, 0, 20, 0});
        const bool debugPane = debugMode && !fullscreen;
        find<UIElement>(L"DebugSidebar").Visibility(debugPane ? Visibility::Visible : Visibility::Collapsed);
        workspace.ColumnSpacing(pane || debugPane ? 12 : 0);
        workspace.ColumnDefinitions().GetAt(0).Width({pane ? 300. : 0., GridUnitType::Pixel});
        workspace.ColumnDefinitions().GetAt(2).Width({debugPane ? 320. : 0., GridUnitType::Pixel});
        fit();
    }
    void fullScreen() {
        fullscreen = !fullscreen;
        layout();
        window.AppWindow().SetPresenter(fullscreen ? Microsoft::UI::Windowing::AppWindowPresenterKind::FullScreen
                                                   : Microsoft::UI::Windowing::AppWindowPresenterKind::Default);
        find<FontIcon>(L"FullscreenIcon").Glyph(fullscreen ? L"\xE73F" : L"\xE740");
        fit();
    }
    void releaseAudio() {
        ++audioGeneration;
        hasAudio = false;
        audioOpened = false;
        audioFailed = false;
        audioEnded = false;
        if (audio) {
            audio.Pause();
            audio.Source(nullptr);
            audio.Close();
            audio = nullptr;
        }
        audioItem = nullptr;
        find<TextBlock>(L"AudioState").Text(L"Bez zvukové stopy · .fajkv ukládá pouze obraz");
    }
    fire_and_forget loadAudio() {
        auto lifetime = get_strong();
        releaseAudio();
        if (!sourceVideo())
            co_return;
        const auto token = audioGeneration;
        auto path = sourcePath;
        try {
            auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(path.wstring());
            if (closing || token != audioGeneration)
                co_return;
            audio = winrt::Windows::Media::Playback::MediaPlayer();
            audio.AutoPlay(false);
            audio.IsVideoFrameServerEnabled(true);
            audio.Volume(find<Slider>(L"Volume").Value() / 100.);
            audioItem = winrt::Windows::Media::Playback::MediaPlaybackItem(
                winrt::Windows::Media::Core::MediaSource::CreateFromStorageFile(file));
            audio.MediaOpened([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get())
                    self->audioOpened = true;
            });
            audio.MediaEnded([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get())
                    self->audioEnded = true;
            });
            audio.MediaFailed([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get())
                    self->audioFailed = true;
            });
            audio.Source(audioItem);
        } catch (...) {
            if (token == audioGeneration)
                audioFailed = true;
        }
    }
    Path dialog(bool save, bool sequence = false, bool png = false) {
        com_ptr<IFileDialog> picker;
        if (save)
            check_hresult(
                CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(picker.put())));
        else
            check_hresult(
                CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(picker.put())));
        const bool image = save && !png && !sourceVideo();
        COMDLG_FILTERSPEC filter{png        ? L"Obrázek PNG"
                                 : save     ? (image ? L"Obrázek Fajk" : L"Video Fajk")
                                 : sequence ? L"Obrázky a videa Fajk"
                                            : L"Videa a obrázky",
                                 png        ? L"*.png"
                                 : save     ? (image ? L"*.fajk" : L"*.fajkv")
                                 : sequence ? L"*.fajk;*.fajkv"
                                            : L"*.mp4;*.mkv;*.mov;*.avi;*.wmv;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff"};
        check_hresult(picker->SetFileTypes(1, &filter));
        check_hresult(picker->SetDefaultExtension(png ? L"png" : image ? L"fajk" : L"fajkv"));
        if (save) {
            auto stem = sourcePath.empty() ? sequencePath.stem().wstring() : sourcePath.stem().wstring();
            auto settings = encodeOptions();
            std::wstring suffix = settings.lossless ? L"_lossless" : L"_q" + std::to_wstring(int(settings.quality));
            if (png) {
                suffix = L"_frame" + std::to_wstring(current + 1);
                if (previewPacket && previewPacket->data.size() > 6)
                    suffix += L"_q" + std::to_wstring(previewPacket->data[6]);
            }
            auto name = stem + suffix + (png ? L".png" : image ? L".fajk" : L".fajkv");
            check_hresult(picker->SetFileName(name.c_str()));
        }
        const auto hr = picker->Show(hwnd);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
            return {};
        check_hresult(hr);
        com_ptr<IShellItem> item;
        check_hresult(picker->GetResult(item.put()));
        PWSTR name = nullptr;
        check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &name));
        Path path(name);
        CoTaskMemFree(name);
        return path;
    }
    void launch(std::function<Result(std::stop_token)> work) {
        stopPlay();
        error(L"");
        busy = true;
        find<ProgressBar>(L"Progress").IsIndeterminate(true);
        updateEnabled();
        if (!job.joinable())
            job = std::jthread([this] {
                for (;;) {
                    std::function<Result(std::stop_token)> work;
                    std::stop_token stop;
                    {
                        std::unique_lock lock(mutex);
                        jobWake.wait(lock, [this] { return jobClosing || bool(pendingJob); });
                        if (jobClosing)
                            return;
                        work = std::move(pendingJob);
                        pendingJob = {};
                        stop = jobStop.get_token();
                    }
                    Result r;
                    try {
                        r = work(stop);
                    } catch (std::exception const& e) {
                        r.error = to_hstring(e.what()).c_str();
                    } catch (hresult_error const& e) {
                        r.error = e.message().c_str();
                    } catch (...) {
                        r.error = L"Neočekávaná chyba při zpracování.";
                    }
                    // Drop captured frames before publishing readiness; a new operation
                    // can then reuse this thread's codec scratch without a thread join.
                    work = {};
                    {
                        std::lock_guard lock(mutex);
                        if (!jobClosing)
                            result = std::move(r);
                    }
                }
            });
        {
            std::lock_guard lock(mutex);
            fajk::require(!jobClosing && !pendingJob, "Worker is not ready.");
            jobStop = std::stop_source{};
            pendingJob = std::move(work);
        }
        jobWake.notify_one();
    }
    void cancelJob() {
        std::stop_source stop;
        {
            std::lock_guard lock(mutex);
            stop = jobStop;
        }
        stop.request_stop();
    }
    void invalidateDecode() {
        ++generation;
        requested = UINT64_MAX;
        if(decoder)decoder->invalidate(generation);
    }
    void clearSequence() {
        ++contentRevision;
        budgetMap.reset();
        budgetFrame.reset();
        budgetUploaded.reset();
        budgetBitmap = nullptr;
        stopPlay();
        invalidateDecode();
        sequencePath.clear();
        index.clear();
        current = 0;
        compressed.reset();
        previewPacket.reset();
        previewOptions.reset();
        previewOriginal.reset();
        compressedUploaded.reset();
        legacyFilter = false;
        colorFilter = false;
        sharpFilter = false;
        filterPending = false;
        find<Border>(L"SizeChangePanel").Visibility(Visibility::Collapsed);
    }
    void resetView() {
        renderedMode = -1;
        releaseAudio();
        original.reset();
        shown.reset();
        originalUploaded.reset();
        find<Image>(L"Picture").Source(nullptr);
        find<Image>(L"Reference").Source(nullptr);
        find<StackPanel>(L"Empty").Visibility(Visibility::Visible);
        for (auto name : {L"Reference", L"Divider", L"CompareLeft", L"CompareRight", L"CompareControls"})
            find<UIElement>(name).Visibility(Visibility::Collapsed);
        fitMode = true;
        settingTimeline = true;
        find<Slider>(L"Timeline").Value(0);
        settingTimeline = false;
    }
    void openSource(Path const& path) {
        if (busy || path.empty())
            return;
        clearSequence();
        resetView();
        sourcePath.clear();
        status(L"Načítání vstupu…");
        launch([path](auto) {
            MediaSource source(path);
            Result r;
            r.path = path;
            r.sourceBytes = std::filesystem::file_size(path);
            r.media = source.info();
            r.frame = std::make_shared<fajk::Frame>();
            fajk::require(source.next(*r.frame), "Input has no frames.");
            return r;
        });
    }
    static Result sequenceResult(Path const& path) {
        SequenceReader reader(path);
        Result r;
        r.kind = Result::Sequence;
        r.path = path;
        r.sequence = reader.info();
        r.index = reader.index();
        auto packet = reader.read(0);
        r.legacyFilter = usesBlockFilter(packet);
        r.colorFilter = usesColorFilter(packet);
        r.sharpFilter = usesSharpFilter(packet);
        return r;
    }
    void openSequence(Path const& path) {
        if (busy || path.empty())
            return;
        clearSequence();
        resetView();
        sourcePath.clear();
        status(L"Otevírání sekvence…");
        launch([path](auto) { return sequenceResult(path); });
    }
    void attach() {
        auto path = dialog(false);
        if (path.empty())
            return;
        auto info = sequenceInfo;
        launch([path, info](auto) {
            MediaSource source(path);
            Result r;
            r.kind = Result::Linked;
            r.path = path;
            r.sourceBytes = std::filesystem::file_size(path);
            r.media = source.info();
            fajk::require(r.media.width == info.width && r.media.height == info.height,
                          "Original and compressed dimensions must match.");
            fajk::require(r.media.image ? info.count == 1 : std::abs(r.media.duration - info.duration) < 10000000,
                          "Original duration does not match this sequence.");
            return r;
        });
    }
    void applyDeblocking() {
        if ((!legacyFilter && !colorFilter && !sharpFilter) || busy)
            return;
        filterPending = false;
        deblocking = uint32_t(find<Slider>(L"Deblocking").Value());
        sharpening = uint32_t(find<Slider>(L"Sharpening").Value());
        saveSettings();
        stopPlay();
        invalidateDecode();
        if (!sequencePath.empty())
            requestFrame(current);
        else if (compressed && original && previewPacket)
            refreshFilter();
        else
            status(L"Filtr uložen pro další náhled.");
    }
    void refreshFilter() {
        filterJob = true;
        auto packet = previewPacket;
        auto frame = original;
        auto threads = options.workers;
        launch([packet, frame, threads, backend=decoderBackend, filter = deblocking, sharpen = sharpening](auto) {
            auto codec = makeCodec(CodecId::Fajk);
            codec->setComputeBackend(backend);
            codec->setThreads(threads);
            codec->setDeblocking(filter);
            codec->setSharpening(sharpen);
            auto start = std::chrono::steady_clock::now();
            Result r;
            r.kind = Result::Preview;
            r.legacyFilter = usesBlockFilter(*packet);
            r.colorFilter = usesColorFilter(*packet);
            r.sharpFilter = usesSharpFilter(*packet);
            r.previewPacket = packet;
            r.size = packet->data.size();
            r.frame = decodeStudio(*codec, *packet, frame->width, frame->height, threads, filter, sharpen);
            r.decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            return r;
        });
    }
    void preview() {
        if (!original || busy)
            return;
        stopPlay();
        invalidateDecode();
        auto frame = original;
        auto settings = encodeOptions();
        settings.threads =
            safeWorkers(frame->width, frame->height, settings.lossless, options.workers, options.memoryMiB);
        const bool captureMaps=debugMode && find<ToggleSwitch>(L"HeatmapToggle").IsOn();
        const bool reusableMap=captureMaps&&budgetMap&&!budgetMap->bits.empty()&&previewOptions&&
                               previewOriginal.lock()==frame&&samePreviewCompression(*previewOptions,settings);
        if (captureMaps&&!reusableMap)
            settings.budgetHeatmap = std::make_shared<BudgetHeatmap>();
        status(L"Výpočet komprimovaného náhledu…");
        std::shared_ptr<Packet> cached;
        if (settings.revision == EncoderRevision::Experimental011441R441 && !settings.budgetHeatmap && previewPacket &&
            previewOptions && previewOriginal.lock() == frame && samePreviewCompression(*previewOptions, settings) &&
            previewPacket->data.size() > 4 && previewPacket->data[4] == reconstruction441Version) {
            cached = std::make_shared<Packet>(*previewPacket);
            update441(*cached, settings.reconstruction441);
        }
        // Reconstruction-only edits reuse both coefficients and their actual
        // allocation map. A changed compression option still captures anew.
        if(cached&&reusableMap)settings.budgetHeatmap=budgetMap;
        else if(captureMaps&&!settings.budgetHeatmap)settings.budgetHeatmap=std::make_shared<BudgetHeatmap>();
        const auto previousSavings = reusableMap ? allocationSavings : std::wstring{};
        launch([frame, settings, cached, previousSavings, backend=decoderBackend, filter = deblocking, sharpen = sharpening](auto stop) {
            auto codec = makeCodec(settings.codec);
            codec->setComputeBackend(backend);
            codec->setThreads(settings.threads);
            codec->setDeblocking(filter);
            codec->setSharpening(sharpen);
            auto start = std::chrono::steady_clock::now();
            auto packet = cached ? *cached : codec->encode(*frame, settings);
            Result r;
            r.encodedOptions = settings;
            r.reusedCompression = bool(cached);
            r.encodeMs =
                cached ? -1
                       : std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            start = std::chrono::steady_clock::now();
            r.kind = Result::Preview;
            r.size = packet.data.size();
            r.legacyFilter = usesBlockFilter(packet);
            r.colorFilter = usesColorFilter(packet);
            r.sharpFilter = usesSharpFilter(packet);
            r.frame = decodeStudio(*codec, packet, frame->width, frame->height, settings.threads, filter, sharpen);
            r.decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (settings.budgetHeatmap && !settings.budgetHeatmap->bits.empty()) {
                r.allocationSavings = previousSavings;
                if (!settings.adaptiveQuantizationEnabled)
                    r.allocationSavings = L"AQ vypnutá: místní váhy jsou neutrální, místní kroky vypnuté. Klasifikace ploch se nepořizuje; bilance úspory se nepoužívá.";
                else if (r.allocationSavings.empty() && !stop.stop_requested()) {
                    auto referenceOptions = settings;
                    if (settings.flatSavings || settings.flatThreshold) {referenceOptions.flatSavings = 0;referenceOptions.flatThreshold=0;}
                    else referenceOptions.flatAllocation = 0;
                    referenceOptions.budgetHeatmap = std::make_shared<BudgetHeatmap>();
                    // Explicit diagnostic work, after both codec timers. No
                    // reference encode or map exists when Debug is disabled.
                    uint64_t referenceBytes = packet.data.size();
                    if (settings.flatAllocation || settings.flatSavings || settings.flatThreshold)
                        referenceBytes = codec->encode(*frame, referenceOptions).data.size();
                    else
                        referenceOptions.budgetHeatmap = settings.budgetHeatmap;
                    if (!referenceOptions.budgetHeatmap->bits.empty()) {
                        r.allocationSavings = to_hstring(flatAllocationSavingsText(
                            compareFlatAllocation(*settings.budgetHeatmap, *referenceOptions.budgetHeatmap), settings.flatSavings != 0 || settings.flatThreshold != 0)).c_str();
                        r.allocationSavings += L"\nCelý soubor: " + bytes(referenceBytes + 168) + L" → " +
                                               bytes(packet.data.size() + 168) + L" (včetně obalů).";
                    }
                }
            }
            r.previewPacket = std::make_shared<Packet>(std::move(packet));
            return r;
        });
    }
    void convertTo(Path const& path) {
        if (path.empty() || busy)
            return;
        auto input = sourcePath;
        requireDifferentFiles(input, path);
        auto config = options;
        config.encode = encodeOptions();
        config.autoChroma = false;
        config.every = sourceVideo() ? every(L"Every") : 1;
        config.temporal=sourceVideo()&&find<ToggleSwitch>(L"Temporal").IsOn();
        config.coefficientTemporal=config.temporal&&find<ComboBox>(L"TemporalMethod").SelectedIndex()==0;
        config.maxCompressionTemporal=config.temporal&&find<ComboBox>(L"TemporalMethod").SelectedIndex()==2;
        const double keyInterval=find<NumberBox>(L"KeyframeInterval").Value();
        config.keyframeInterval=std::isfinite(keyInterval)?uint32_t(std::clamp(keyInterval,0.,double(maxFrames))):120;
        const double tolerance=find<NumberBox>(config.maxCompressionTemporal?L"MaxTolerance":L"SimilarTolerance").Value(),hold=find<NumberBox>(L"SimilarHold").Value();
        config.similarTolerance=std::isfinite(tolerance)?uint32_t(std::clamp(tolerance,0.,16.)):0;
        if(config.coefficientTemporal)config.similarTolerance=0;
        config.similarHoldFrames=std::isfinite(hold)?uint32_t(std::clamp(hold,1.,120.)):10;
        config.replace = true;
        if (sourceInfo.image && original) {
            auto frame = original;
            auto cached = previewPacket;
            const auto& wanted = config.encode;
            if (!previewOptions || previewOptions->reconstruction441 != wanted.reconstruction441 ||
                previewOriginal.lock() != frame || !samePreviewCompression(*previewOptions,wanted))
                cached.reset();
            auto cachedBudget = cached ? budgetFrame : nullptr;
            clearSequence();
            budgetFrame = cachedBudget;
            status(cached ? L"Ukládání vypočítaného náhledu…" : L"Komprese obrázku…");
            launch([frame, cached, path, config](auto stop) {
                fajk::require(!stop.stop_requested(), "Conversion cancelled.");
                auto settings = config.encode;
                settings.threads =
                    safeWorkers(frame->width, frame->height, settings.lossless, config.workers, config.memoryMiB);
                Packet packet;
                if (!cached) {
                    auto codec = makeCodec(settings.codec);
                    packet = codec->encode(*frame, settings);
                }
                const auto& output = cached ? *cached : packet;
                fajk::require(!stop.stop_requested(), "Conversion cancelled.");
                SequenceInfo info;
                info.width = frame->width;
                info.height = frame->height;
                info.fpsNum = 1;
                SequenceWriter writer(path, info, true);
                writer.append(output);
                fajk::require(!stop.stop_requested(), "Conversion cancelled.");
                writer.finish();
                auto r = sequenceResult(path);
                r.kind = Result::Converted;
                r.size = writer.bytesWritten();
                return r;
            });
            return;
        }
        clearSequence();
        status(L"Komprese probíhá…");
        launch([this, input, path, config](auto stop) {
            auto p = convert(input, path, config, stop, [this](Progress const& p) {
                std::lock_guard lock(mutex);
                progress = p;
            });
            auto r = sequenceResult(path);
            r.kind = Result::Converted;
            r.size = p.bytes;
            return r;
        });
    }
    void exportFrame() {
        if (!shown || busy)
            return;
        auto path = dialog(true, false, true);
        if (path.empty())
            return;
        if (!sourcePath.empty())
            requireDifferentFiles(sourcePath, path);
        if (!sequencePath.empty())
            requireDifferentFiles(sequencePath, path);
        auto frame = shown;
        launch([frame, path](auto) {
            writePng(path, *frame);
            Result r;
            r.kind = Result::Exported;
            r.path = path;
            return r;
        });
    }
    void startDecoder() {
        decoder=std::make_unique<PlaybackDecoder>(smoke?executableDirectory()/L"decode-cache-smoke.txt":Path{});
        decoder->invalidate(generation);
    }
    void requestFrame(uint64_t i) {
        if (!playable() && index.empty())
            return;
        i = std::min(i, lastFrame());
        if (requested == i)
            return;
        requested = i;
        DecodeRequest work;
        work.path = sequencePath;
        work.source = sourcePath;
        work.frame = i;
        work.generation = generation;
        work.contentRevision = contentRevision;
        work.pts = frameTime(i);
        work.threads = options.workers;
        work.memoryMiB = options.memoryMiB;
        work.backend=decoderBackend;
        work.deblocking = deblocking;
        work.sharpening = sharpening;
        work.last = lastFrame();
        work.step = every(L"Step");
        work.fpsNum = sourceInfo.fpsNum;
        work.fpsDen = sourceInfo.fpsDen;
        work.mode = find<ComboBox>(L"ViewMode").SelectedIndex();
        work.continuous = playing;
        decoder->submit(std::move(work));
    }
    void stopPlay() {
        if (playing) {
            playing = false;
            invalidateDecode();
        }
        if (audio)
            audio.Pause();
        if (root) {
            find<FontIcon>(L"PlayIcon").Glyph(L"\xE768");
            Automation::AutomationProperties::SetName(find<Button>(L"Play"), L"Přehrát");
        }
    }
    void play() {
        if (busy)
            return;
        if (playing) {
            stopPlay();
            return;
        }
        if (!playable())
            return;
        if (current >= lastFrame())
            current = 0;
        invalidateDecode();
        clockBase = frameTime(current);
        clockStart = std::chrono::steady_clock::now();
        playing = true;
        requestFrame(current);
        find<FontIcon>(L"PlayIcon").Glyph(L"\xE769");
        Automation::AutomationProperties::SetName(find<Button>(L"Play"), L"Pozastavit");
        if (hasAudio && audio) {
            audio.PlaybackSession().Position(TimeSpan{clockBase});
            audio.PlaybackSession().PlaybackRate(speed());
            audio.Play();
        }
    }
    void step(int direction) {
        const auto base = requested == UINT64_MAX ? current : requested;
        stopPlay();
        if (!playable())
            return;
        invalidateDecode();
        requestFrame(uint64_t(
            std::clamp<int64_t>(int64_t(base) + direction * int64_t(every(L"Step")), 0, int64_t(lastFrame()))));
    }
    int64_t playbackTime() {
        if (hasAudio && audio)
            return audio.PlaybackSession().Position().count();
        return clockBase +
               int64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - clockStart).count() * 1e7 *
                       speed());
    }
    void describeEncoder() {
        const auto selected = find<ComboBox>(L"CodecVersion").SelectedIndex();
        const auto& entry =
            encoderCatalog[selected >= 0 && size_t(selected) < encoderCatalog.size() ? size_t(selected) : 0];
        find<StackPanel>(L"R441Controls")
            .Visibility(entry.revision == EncoderRevision::Experimental011441R441 ? Visibility::Visible
                                                                                  : Visibility::Collapsed);
        find<TextBlock>(L"EncoderSummary").Text(entry.summary);
        const bool gpu=entry.backend==ComputeBackend::Gpu||decoderBackend==ComputeBackend::Gpu;
        find<TextBlock>(L"ComputeStatus").Visibility(gpu?Visibility::Visible:Visibility::Collapsed);
        if(gpu){if(!computeDevice)computeDevice=gpuInfo();
            find<TextBlock>(L"ComputeStatus").Text(computeDevice->available?to_hstring(computeDevice->device)+L" · CPU + GPU":L"GPU není dostupná. Použije se CPU.");}
        find<StackPanel>(L"ShapeControls").Visibility(Visibility::Visible);
        find<Expander>(L"ContourControls").Visibility(Visibility::Visible);
    }
    void cycleDebugView(int slot, int wheelDelta) {
        auto box = find<ComboBox>(slot ? L"DebugRight" : L"DebugLeft");
        debugWheelRemainder[slot] += wheelDelta;
        const int steps = debugWheelRemainder[slot] / 120;
        debugWheelRemainder[slot] %= 120;
        if (steps)
            box.SelectedIndex(std::clamp(box.SelectedIndex() - steps, 0, int(debugViewNames.size()) - 1));
    }
    fire_and_forget debugViewHelp(int slot) {
        auto lifetime = get_strong();
        try {
            const auto view = find<ComboBox>(slot ? L"DebugRight" : L"DebugLeft").SelectedIndex();
            if (view < 0 || view >= int(debugViewNames.size()))
                co_return;
            ContentDialog dialog;
            dialog.XamlRoot(root.XamlRoot());
            dialog.Title(box_value(debugViewNames[view]));
            dialog.CloseButtonText(L"Zavřít");
            TextBlock description;
            description.TextWrapping(TextWrapping::Wrap);
            description.MaxWidth(620);
            description.Text(std::wstring(debugViewDescriptions[view]) + L"\n\n" +
                             (debugCosts[view].empty() ? L"Cena aktuálního snímku bude dostupná po dokončení analýzy."
                                                       : to_hstring(debugCosts[view]).c_str()));
            ScrollViewer scroll;
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.MaxHeight(560);
            scroll.Content(description);
            dialog.Content(scroll);
            co_await dialog.ShowAsync();
        } catch (hresult_error const& e) {
            error(e.message().c_str());
        }
    }
    fire_and_forget encoderHelp() {
        auto lifetime = get_strong();
        try {
            ContentDialog dialog;
            dialog.XamlRoot(root.XamlRoot());
            dialog.Title(box_value(L"Přehled enkodérů FAJK"));
            dialog.CloseButtonText(L"Zavřít");
            StackPanel list;
            list.Spacing(18);
            list.MaxWidth(680);
            TextBlock intro;
            intro.Text(
                L"R600.4 nabízí společnou volbu CPU nebo GPU pro enkodér, náhled a přehrávání. "
                L"Výchozí Skirt 50 %, kontury 200 %, plochy vypnuté. Doostření hran funguje i při vyšší kvalitě. "
                L"Rychlá rekonstrukce nahrazuje pomalý referenční filtr. Enkodér vybírá postup podle obsahu a "
                L"rozpočtu; "
                L"kvalita a limit velikosti zůstávají hlavním ovládáním.");
            intro.TextWrapping(TextWrapping::Wrap);
            list.Children().Append(intro);
            for (const auto& entry : encoderCatalog) {
                StackPanel item;
                item.Spacing(5);
                TextBlock title;
                title.Text(entry.name);
                title.FontSize(16);
                TextBlock text;
                text.Text(entry.details);
                text.TextWrapping(TextWrapping::Wrap);
                item.Children().Append(title);
                item.Children().Append(text);
                list.Children().Append(item);
            }
            ScrollViewer scroll;
            scroll.MaxHeight(540);
            scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.Content(list);
            dialog.Content(scroll);
            DispatcherTimer closeTimer{nullptr};
            winrt::event_token closeEvent{};
            if (smoke) {
                closeTimer = DispatcherTimer();
                closeTimer.Interval(std::chrono::milliseconds(100));
                closeEvent = closeTimer.Tick([dialog, closeTimer](auto&&, auto&&) {
                    closeTimer.Stop();
                    dialog.Hide();
                });
            }
            auto helpOperation = dialog.ShowAsync();
            if (smoke)
                closeTimer.Start();
            co_await helpOperation;
            if (smoke) {
                closeTimer.Tick(closeEvent);
                std::ofstream log(executableDirectory() / L"ui-smoke.txt", std::ios::app);
                log << "encoder catalog dialog PASS\n";
            }
        } catch (hresult_error const& e) {
            error(e.message().c_str());
        }
    }
    void updateSkirtStrength() {
        auto settings=encodeOptions();
        const auto raw=uint32_t(find<Slider>(L"R441Intensity").Value());
        find<TextBlock>(L"R441IntensityValue").Text(std::to_wstring(raw)+L" %");
        auto reconstruction=settings.reconstruction441;
        uint32_t budget=0;
        bool actual=false;
        // An old preview must not masquerade as the result of new controls.
        if(previewPacket&&previewOptions&&original&&previewOriginal.lock()==original&&
           previewOptions->reconstruction441==reconstruction&&samePreviewCompression(*previewOptions,settings)&&
           previewPacket->data.size()>=reconstruction441HeaderBytes&&previewPacket->data[4]==reconstruction441Version){
            const auto stored=read441(previewPacket->data,original->width,original->height);
            reconstruction=stored.settings;budget=stored.budget;actual=true;
        }else if(sourceInfo.width&&sourceInfo.height){
            budget=settings.automaticTarget?qualityTargetBytes(sourceInfo.width,sourceInfo.height,settings.quality):settings.targetBytes;
        }
        float amount=std::min(1.f,reconstruction.intensity/100.f);
        if(budget&&sourceInfo.width&&sourceInfo.height)
            amount=skirtBlend(reconstruction,cleanupQuality(budget,sourceInfo.width,sourceInfo.height));
        const bool upperBound=!budget&&!reconstruction.fixedIntensity&&reconstruction.intensity;
        find<TextBlock>(L"R441ActualValue").Text(std::wstring(actual?L"Použito: ":L"Odhad: ")+
            (upperBound?L"≤ ":L"")+number(amount*100,1)+L" %");
        find<TextBlock>(L"CleanupHint").Text(reconstruction.fixedIntensity
            ?L"Ruční režim: bez automatického útlumu kvalitou i hustotou dat."
            :L"Automaticky: od kvality 10 slábne, při 50 je Skirt / dering vypnutý. Zohledňuje i hustotu dat.");
    }
    void settingsChanged() {
        describeEncoder();
        find<TextBlock>(L"QualityValue").Text(std::to_wstring(int(find<Slider>(L"Quality").Value())) + L" / 100");
        auto settings = encodeOptions();
        auto target = settings.automaticTarget && !sourcePath.empty()
                          ? qualityTargetBytes(sourceInfo.width, sourceInfo.height, settings.quality)
                          : settings.targetBytes;
        find<TextBlock>(L"BudgetHint")
            .Text(target ? L"Cíl: " + bytes(target) + L" na snímek."
                         : L"Velikost závisí na obsahu a kvalitě. Pro 2K automatický cíl přibližně 20–200 kB.");
        updateSkirtStrength();
        find<TextBlock>(L"Estimate").Text(L"Změna nastavení se projeví po výpočtu nového náhledu.");
        if (debugMode && budgetMap && previewOptions && !samePreviewCompression(*previewOptions, settings))
            find<TextBlock>(L"DebugPixelProbe")
                .Text(L"Mapa odpovídá předchozímu náhledu. Po změně rozpočtu / vah přepočítejte mapy.");
        find<Border>(L"SizeChangePanel").Visibility(Visibility::Collapsed);
        if (debugMode && original && sequencePath.empty()) {
            debugEncodePending = true;
            requestDebug();
        }
        updateEnabled();
        saveSettings();
    }
    void smokeSkirtControls() {
        const auto quality=find<Slider>(L"Quality").Value();
        const auto intensity=find<Slider>(L"R441Intensity").Value();
        const bool automatic=find<CheckBox>(L"R441Automatic").IsChecked().Value();
        const auto shapes=encodeOptions().reconstruction441.shapes;
        bool passed=true;
        find<Slider>(L"R441Intensity").Value(50);
        find<CheckBox>(L"R441Automatic").IsChecked(true);
        for(auto [q,expected]:{std::pair{10,50},std::pair{30,25},std::pair{50,0},std::pair{100,0}}){
            find<Slider>(L"Quality").Value(q);
            const auto controls=encodeOptions().reconstruction441;
            passed=passed&&controls.intensity==uint32_t(expected)&&!controls.fixedIntensity;
        }
        find<CheckBox>(L"R441Automatic").IsChecked(false);
        passed=passed&&encodeOptions().reconstruction441.intensity==50&&encodeOptions().reconstruction441.fixedIntensity&&
            std::wstring(find<TextBlock>(L"R441ActualValue").Text())==L"Odhad: 50.0 %";
        saveSettings();
        passed=passed&&GetPrivateProfileIntW(L"Studio",L"r441Intensity",0,settingsPath().c_str())==50&&
            GetPrivateProfileIntW(L"Studio",L"r441Automatic",1,settingsPath().c_str())==0;
        find<Slider>(L"ShapeTolerance").Value(175);
        find<Slider>(L"ShapeMinimumSize").Value(50);
        passed=passed&&encodeOptions().reconstruction441.shapes==ContourTuning{175,50};
        find<Slider>(L"ShapeTolerance").Value(100);
        find<Slider>(L"ShapeMinimumSize").Value(100);
        passed=passed&&encodeOptions().reconstruction441.shapes==ContourTuning{};
        find<Slider>(L"Quality").Value(quality);
        find<Slider>(L"R441Intensity").Value(intensity);
        find<CheckBox>(L"R441Automatic").IsChecked(automatic);
        find<Slider>(L"ShapeTolerance").Value(shapes.tolerance);
        find<Slider>(L"ShapeMinimumSize").Value(shapes.minimumSize);
        saveSettings();
        std::ofstream(executableDirectory()/L"ui-smoke.txt",std::ios::app)
            <<"Skirt quality fade, manual override and raw settings persistence "<<(passed?"PASS":"ERROR")<<'\n';
    }
    Path settingsPath() { return executableDirectory().parent_path() / L"settings.ini"; }
    void saveSettings() {
        auto path = settingsPath();
        const auto encoded = encodeOptions();
        auto put = [&](const wchar_t* key, uint64_t v) {
            const auto previous = settingsWritten.find(key);
            if (previous != settingsWritten.end() && previous->second == v)
                return;
            if (WritePrivateProfileStringW(L"Studio", key, std::to_wstring(v).c_str(), path.c_str()))
                settingsWritten[key] = v;
        };
        put(L"r441Intensity", uint32_t(find<Slider>(L"R441Intensity").Value()));
        put(L"r441Automatic", !encoded.reconstruction441.fixedIntensity);
        put(L"shapeTolerance",encoded.reconstruction441.shapes.tolerance);
        put(L"shapeMinimumSize",encoded.reconstruction441.shapes.minimumSize);
        put(L"r441Tangents", encoded.reconstruction441.strongerTangents);
        put(L"r441AA", encoded.reconstruction441.antialias);
        put(L"r441Contrast", encoded.reconstruction441.contrast);
        put(L"r441Style", encoded.reconstruction441.style);
        put(L"r441TextProtection", encoded.reconstruction441.protectText);
        put(L"adaptiveQuantizationEnabled",encoded.adaptiveQuantizationEnabled);
        put(L"reconstructionVersion", 341);
        put(L"shapeIntensity", encoded.shapeIntensity);
        put(L"contourIntensity", encoded.contourIntensity);
        put(L"flatAllocation", encoded.flatAllocation);
        put(L"flatSavings", encoded.flatSavings);
        put(L"flatThreshold", encoded.flatThreshold);
        put(L"chroma420", encoded.chroma420);
        put(L"chroma422", encoded.chroma422);
        put(L"quality", uint64_t(find<Slider>(L"Quality").Value()));
        put(L"budget", std::max(0, find<ComboBox>(L"Budget").SelectedIndex()));
        put(L"budgetVersion", 3);
        put(L"every", every(L"Every"));
        put(L"ram", options.memoryMiB);
        put(L"workers", options.workers);
        put(L"resourcesVersion", 3);
        if (!retiredSettingsRemoved)
            retiredSettingsRemoved =
                WritePrivateProfileStringW(L"Studio", L"fastDecoding", nullptr, path.c_str()) != FALSE;
        put(L"deblocking", deblocking);
        put(L"sharpening", sharpening);
        put(L"volume", uint64_t(find<Slider>(L"Volume").Value()));
        put(L"lossless", 0);
        put(L"preset", 0);
        put(L"codecRevision", uint32_t(encoded.revision));
        put(L"encodeBackend",uint32_t(encoded.backend));
        put(L"encodeSpeed",uint32_t(encoded.speed));
        put(L"temporal",find<ToggleSwitch>(L"Temporal").IsOn());
        put(L"coefficientTemporal",find<ComboBox>(L"TemporalMethod").SelectedIndex()==0);
        put(L"temporalMethod",uint32_t(std::max(0,find<ComboBox>(L"TemporalMethod").SelectedIndex())));
        const double keyInterval=find<NumberBox>(L"KeyframeInterval").Value();
        put(L"keyframeInterval",std::isfinite(keyInterval)?uint32_t(std::clamp(keyInterval,0.,double(maxFrames))):120);
        const double tolerance=find<NumberBox>(L"SimilarTolerance").Value(),hold=find<NumberBox>(L"SimilarHold").Value();
        put(L"similarTolerance",std::isfinite(tolerance)?uint32_t(std::clamp(tolerance,0.,16.)):0);
        put(L"similarHold",std::isfinite(hold)?uint32_t(std::clamp(hold,1.,120.)):10);
        const double maxTolerance=find<NumberBox>(L"MaxTolerance").Value();
        put(L"maxTolerance",std::isfinite(maxTolerance)?uint32_t(std::clamp(maxTolerance,0.,16.)):6);
        put(L"decodeBackend",uint32_t(encoded.backend));
    }
    fire_and_forget settings() {
        auto lifetime = get_strong();
        stopPlay();
        try {
            auto hw = hardwareInfo();
            ContentDialog d;
            d.XamlRoot(root.XamlRoot());
            d.Title(box_value(L"Kvalita a výkon"));
            d.PrimaryButtonText(L"Uložit");
            d.CloseButtonText(L"Zrušit");
            d.DefaultButton(ContentDialogButton::Primary);
            StackPanel panel;
            panel.Spacing(14);
            panel.MaxWidth(460);
            TextBlock hardware;
            hardware.Text(std::to_wstring(hw.physical) + L" jader · " + std::to_wstring(hw.logical) +
                          L" logických procesorů\nRAM " + number(hw.totalMiB / 1024.) + L" GB · dostupné " +
                          number(hw.availableMiB / 1024.) + L" GB");
            panel.Children().Append(hardware);
            ToggleSwitch automatic;
            automatic.Header(box_value(L"Automatický výkon · vysoký"));
            automatic.IsOn(options.workers == 0);
            panel.Children().Append(automatic);
            TextBlock count;
            count.Text(std::to_wstring(workerLimit(options.workers)) + L" pracovních vláken");
            panel.Children().Append(count);
            Slider workers;
            workers.Minimum(1);
            workers.Maximum(hw.logical);
            workers.StepFrequency(1);
            workers.Value(workerLimit(options.workers));
            workers.IsEnabled(!automatic.IsOn());
            panel.Children().Append(workers);
            Button ultra;
            ultra.Content(box_value(L"Ultra · použít všechna dostupná vlákna"));
            panel.Children().Append(ultra);
            automatic.Toggled([workers](auto const& sender, auto&&) {
                bool enabled = sender.template as<ToggleSwitch>().IsOn();
                workers.IsEnabled(!enabled);
                if (enabled)
                    workers.Value(workerLimit());
            });
            workers.ValueChanged([count](auto&&, auto const& e) {
                count.Text(std::to_wstring(int(e.NewValue())) + L" pracovních vláken");
            });
            ultra.Click([automatic, workers, hw](auto&&, auto&&) {
                automatic.IsOn(false);
                workers.Value(hw.logical);
            });
            ToggleSwitch autoRam;
            autoRam.Header(box_value(L"Automatická správa RAM"));
            autoRam.IsOn(options.memoryMiB == 0);
            panel.Children().Append(autoRam);
            NumberBox ram;
            ram.Header(box_value(L"Horní limit pracovních dat (MiB)"));
            ram.Minimum(128);
            ram.Maximum(double(std::max<uint64_t>(128, hw.safeMiB)));
            ram.Value(double(options.memoryMiB ? std::min(options.memoryMiB, hw.safeMiB) : hw.safeMiB));
            ram.IsEnabled(!autoRam.IsOn());
            panel.Children().Append(ram);
            autoRam.Toggled(
                [ram](auto const& sender, auto&&) { ram.IsEnabled(!sender.template as<ToggleSwitch>().IsOn()); });
            TextBlock note;
            note.Text(L"Vysoký výkon ponechá 2 logická vlákna systému (na malém CPU alespoň jedno). Ultra je využije "
                      L"také. RAM i souběžnost se před převodem přepočítají podle dostupné paměti, rozlišení a režimu "
                      L"komprese. Obrázky i video využívají více jader.");
            note.TextWrapping(TextWrapping::Wrap);
            panel.Children().Append(note);
            ScrollViewer scroll;
            scroll.Content(panel);
            scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
            scroll.MaxHeight(600);
            d.Content(scroll);
            if (co_await d.ShowAsync() == ContentDialogResult::Primary) {
                fajk::require(autoRam.IsOn() || std::isfinite(ram.Value()), "Enter a valid memory limit.");
                options.memoryMiB =
                    autoRam.IsOn()
                        ? 0
                        : uint64_t(std::clamp(ram.Value(), 128., double(std::max<uint64_t>(128, hw.safeMiB))));
                options.workers = automatic.IsOn() ? 0 : uint32_t(workers.Value());
                settingsChanged();
            }

        } catch (hresult_error const& e) {
            error(e.message().c_str());
        } catch (std::exception const& e) {
            error(to_hstring(e.what()).c_str());
        }
    }
    fire_and_forget drop(DragEventArgs args) {
        auto lifetime = get_strong();
        if (busy)
            co_return;
        auto defer = args.GetDeferral();
        try {
            auto files = co_await args.DataView().GetStorageItemsAsync();
            if (files.Size()) {
                Path path(files.GetAt(0).Path().c_str());
                if (!closing && !busy) {
                    if (fajkFile(path))
                        openSequence(path);
                    else
                        openSource(path);
                }
            }
        } catch (hresult_error const& e) {
            error(e.message().c_str());
        } catch (std::exception const& e) {
            error(to_hstring(e.what()).c_str());
        }
        defer.Complete();
    }
    void shutdown() {
        if (closing)
            return;
        closing = true;
        if (audio) {
            audio.Pause();
            audio.Close();
            audio = nullptr;
        }
        if (timer)
            timer.Stop();
        if (renderingToken.value)
            CompositionTarget::Rendering(renderingToken);
        std::stop_source stop;
        {
            std::lock_guard lock(mutex);
            stop = jobStop;
            jobClosing = true;
            pendingJob = {};
        }
        stop.request_stop();
        jobWake.notify_one();
        if(decoder)decoder->shutdown();
        debugWorker.request_stop();
        if (debugWorker.joinable())
            debugWorker.join();
        if (job.joinable())
            job.join();

    }
    ~Studio() { shutdown(); }
    void beginSharpnessSmoke() {
        if (!sharpFilter) {
            smokeStage = 2;
            return;
        }
        smokeFiltered = compressed;
        smokeSharpening = sharpening;
        smokeTargetSharp = sharpening == 200 ? 100 : 200;
        SequenceReader reader(sequencePath);
        smokePacket = std::make_shared<Packet>(reader.read(0));
        find<Slider>(L"Sharpening").Value(smokeTargetSharp);
        smokeStage = 45;
    }
    fire_and_forget captureSmoke() {
        auto lifetime = get_strong();
        try {
            RenderTargetBitmap bitmap;
            co_await bitmap.RenderAsync(root);
            auto buffer = co_await bitmap.GetPixelsAsync();
            uint8_t* data = nullptr;
            check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&data));
            fajk::Frame frame{uint32_t(bitmap.PixelWidth()), uint32_t(bitmap.PixelHeight()), 0, 1,
                              Bytes(pixelBytes(bitmap.PixelWidth(), bitmap.PixelHeight()))};
            for (size_t i = 0; i < frame.pixels.size(); i += 4) {
                frame.pixels[i] = data[i + 2];
                frame.pixels[i + 1] = data[i + 1];
                frame.pixels[i + 2] = data[i];
                frame.pixels[i + 3] = data[i + 3];
            }
            writePng(executableDirectory() / L"ui-smoke.png", frame);
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app) << "render PASS\n";
        } catch (hresult_error const& e) {
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app)
                << "render ERROR " << to_string(e.message());
        } catch (std::exception const& e) {
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app) << "render ERROR " << e.what();
        }
        window.Close();
    }
    void tick() {
        if (debugMode && debugPending && !busy && !debugInFlight &&
            std::chrono::steady_clock::now() - debugChanged >= std::chrono::milliseconds(debugEncodePending ? 220 : 60))
            inspectDebug();
        if (smokeDebug && smokeStage == 90 && !busy &&
            std::chrono::steady_clock::now() - smokeStart > std::chrono::milliseconds(500)) {
            smokeStage = 91;
            captureSmoke();
        }
        if (filterPending && !busy && std::chrono::steady_clock::now() - filterChanged >= std::chrono::milliseconds(60))
            applyDeblocking();
        if (audioEnded.exchange(false) && playing) {
            stopPlay();
            requestFrame(lastFrame());
        }
        if (audioOpened.exchange(false) && audioItem) {
            hasAudio = audioItem.AudioTracks().Size() > 0;
            find<TextBlock>(L"AudioState")
                .Text(hasAudio ? L"Zvuk z originálu · hlasitost a ztlumení" : L"Originál neobsahuje zvukovou stopu");
            updateEnabled();
        }
        if (audioFailed.exchange(false)) {
            hasAudio = false;
            find<TextBlock>(L"AudioState").Text(L"Zvuk originálu se nepodařilo otevřít");
            updateEnabled();
        }
        std::optional<Result> ready;
        std::optional<Progress> p;
        {
            std::lock_guard lock(mutex);
            ready = std::move(result);
            result.reset();
            if (!ready && debugResult) {
                ready = std::move(debugResult);
                debugResult.reset();
                debugInFlight = false;
            }
            p = std::move(progress);
            progress.reset();
        }
        if (p) {
            auto bar = find<ProgressBar>(L"Progress");
            bar.IsIndeterminate(!p->duration);
            if (p->duration)
                bar.Value(std::clamp(100. * p->position / p->duration, 0., 100.));
            status(std::to_wstring(p->frames) + L" snímků · " + bytes(p->bytes) + L" · " +
                   number(p->frames / std::max(.001, p->elapsed)) + L" sn./s · až " + std::to_wstring(p->workers) +
                   L" vláken");
        }
        if (ready) {
            auto& r = *ready;
            if (r.kind != Result::Debug) {
                busy = false;
                filterJob = false;
                find<ProgressBar>(L"Progress").IsIndeterminate(false);
                find<ProgressBar>(L"Progress").Value(0);
            }
            if (r.kind == Result::Debug && (!debugMode || r.debugEpoch != debugEpoch))
                return;
            if (!r.error.empty()) {
                error(r.error);
                status(L"Operace nebyla dokončena.");
            } else if (r.kind == Result::Debug) {
                if (debugMode && r.debug && r.debugEpoch == debugEpoch) {
                    debugCosts = r.debug->costs;
                    debugFrame = std::make_shared<fajk::Frame>(std::move(r.debug->comparison));
                    debugUploaded.reset();
                    renderedMode = -1;
                    find<TextBox>(L"DebugReport").Text(to_hstring(r.debug->report));
                    find<TextBlock>(L"DebugViewLegend").Text(to_hstring(r.debug->legend));
                    const auto left = find<ComboBox>(L"DebugLeft").SelectedIndex(),
                               right = find<ComboBox>(L"DebugRight").SelectedIndex();
                    find<TextBlock>(L"DebugCaption")
                        .Text(std::wstring(L"VLEVO: ") + debugViewNames[left] + L" · " +
                              to_hstring(r.debug->captions[0]).c_str() + L"\nVPRAVO: " + debugViewNames[right] +
                              L" · " + to_hstring(r.debug->captions[1]).c_str());
                    render();
                    status(L"Debug snímek připraven. Analýza je oddělená od měření kodeku.");
                }
            } else if (r.kind == Result::Source) {
                ++contentRevision;
                invalidateDebug();
                sourcePath = r.path;
                sourceInfo = r.media;
                originalFileBytes = r.sourceBytes;
                original = r.frame;
                originalUploaded.reset();
                settingsChanged();
                find<TextBlock>(L"Source").Text(sourcePath.filename().wstring() + L"\n" +
                                                std::to_wstring(r.media.width) + L" × " +
                                                std::to_wstring(r.media.height) + L" · " + bytes(originalFileBytes));
                find<TextBlock>(L"ViewTitle").Text(sourcePath.filename().wstring());
                find<Slider>(L"Timeline").Maximum(double(std::max<uint64_t>(1, lastFrame())));
                setMode(0);
                loadAudio();
                status(L"Vstup načten. Porovnat lze právě zobrazený snímek.");
            } else if (r.kind == Result::Preview) {
                invalidateDebug();
                previewPacket = r.previewPacket;
                if (r.encodedOptions) {
                    previewOptions = r.encodedOptions;
                    previewOriginal = original;
                }
                compressed = r.frame;
                updateSkirtStrength();
                legacyFilter = r.legacyFilter;
                colorFilter = r.colorFilter;
                sharpFilter = r.sharpFilter;
                compressedUploaded.reset();
                if (r.encodedOptions) {
                    budgetMap = r.encodedOptions->budgetHeatmap;
                    allocationSavings = r.allocationSavings;
                    find<TextBlock>(L"FlatSavingsReport").Text(allocationSavings.empty()
                        ? L"Srovnání vyžaduje zdrojový obrázek a mapu dat; přesné paletové plochy nemají waveletové detaily."
                        : allocationSavings);
                    rebuildBudgetMap();
                }
                setMode(find<ToggleSwitch>(L"HeatmapToggle").IsOn() ? 1 : 2);
                std::wstring text = (sourceInfo.image ? L"Soubor: " : L"Snímek: ") +
                                    bytes(r.size + (sourceInfo.image ? 168 : 0)) + L"\n" +
                                    (r.reusedCompression ? L"Komprese: uložený náhled\n"
                                     : r.encodeMs >= 0   ? std::wstring(r.encodedOptions&&r.encodedOptions->budgetHeatmap
                                                                                           ? L"Komprese s diagnostikou " : L"Komprese ") + number(r.encodeMs) + L" ms\n"
                                                         : L"") +
                                    L"Dekódování včetně filtrů: " + number(r.decodeMs) + L" ms";
                if (!sourceInfo.image && sourceInfo.duration > 0) {
                    double count = sourceInfo.duration / 1e7 * sourceInfo.fpsNum / sourceInfo.fpsDen / every(L"Every");
                    text += L"\nOdhad videa z jednoho snímku: " + bytes(uint64_t(count * r.size)) + L" (orientační).";
                }
                find<TextBlock>(L"Estimate").Text(text);
                showSizeChange(sourceInfo.image ? originalFileBytes : original->pixels.size(),
                               r.size + (sourceInfo.image ? 168 : 0),
                               sourceInfo.image ? L"Původní obrázek → .fajk (náhled)"
                                                : L"Jeden snímek · RGBA → komprimovaný obraz");
                status(L"Táhněte srovnávací čáru. Kolečko přibližuje v místě kurzoru, tažení posouvá obraz.");
            } else if (r.kind == Result::Sequence || r.kind == Result::Converted) {
                ++contentRevision;
                invalidateDebug();
                sequencePath = r.path;
                sequenceInfo = r.sequence;
                index = std::move(r.index);
                legacyFilter = r.legacyFilter;
                colorFilter = r.colorFilter;
                sharpFilter = r.sharpFilter;
                invalidateDecode();
                current = 0;
                compressed.reset();
                previewPacket.reset();
                compressedUploaded.reset();
                if (r.kind == Result::Sequence) {
                    original.reset();
                    originalUploaded.reset();
                }
                find<Slider>(L"Timeline").Maximum(double(std::max<uint64_t>(1, lastFrame())));
                find<TextBlock>(L"ViewTitle").Text(sequencePath.filename().wstring());
                settingMode = true;
                find<ComboBox>(L"ViewMode").SelectedIndex(1);
                settingMode = false;
                if (r.kind == Result::Converted) {
                    find<TextBlock>(L"Estimate").Text(L"Uloženo: " + bytes(r.size));
                    find<ProgressBar>(L"Progress").Value(100);
                    showSizeChange(originalFileBytes, r.size, L"Celý původní soubor → uložený soubor");
                } else
                    find<TextBlock>(L"Source").Text(L"Pro srovnání a zvuk připojte odpovídající originál.");
                requestFrame(0);
                status(L"Sekvence načtena.");
            } else if (r.kind == Result::Linked) {
                ++contentRevision;
                sourcePath = r.path;
                sourceInfo = r.media;
                originalFileBytes = r.sourceBytes;
                showSizeChange(originalFileBytes, std::filesystem::file_size(sequencePath),
                               L"Celý původní soubor → uložený soubor");
                invalidateDecode();
                loadAudio();
                requestFrame(current);
                status(L"Originál připojen. Ověřte, že jde o stejné video.");
            } else if (r.kind == Result::Exported)
                status(L"Snímek uložen: " + r.path.wstring());
            updateEnabled();
            if (smokeDebug) {
                std::ofstream log(executableDirectory() / L"ui-smoke-debug.txt", std::ios::app);
                if (!r.error.empty()) {
                    log << "ERROR " << to_string(r.error) << '\n';
                    window.Close();
                    return;
                }
                if (r.kind == Result::Source && smokeStage == 0) {
                    const auto savedChroma = find<ComboBox>(L"ChromaMode").SelectedIndex();
                    const auto firstOptions = encodeOptions();
                    find<ComboBox>(L"ChromaMode").SelectedIndex(savedChroma == 0 ? 1 : 0);
                    const auto secondOptions = encodeOptions();
                    log << "chroma selector invalidates compression "
                        << (firstOptions.chroma420 != secondOptions.chroma420 &&
                                    !samePreviewCompression(firstOptions, secondOptions)
                                ? "PASS"
                                : "ERROR")
                        << '\n';
                    find<ComboBox>(L"ChromaMode").SelectedIndex(savedChroma);
                    log << "debug initially off " << (!debugMode && !encodeOptions().budgetHeatmap ? "PASS" : "ERROR")
                        << '\n';
                    smokeStage = 1;
                    preview();
                } else if (r.kind == Result::Preview && smokeStage == 1) {
                    smokePacket = std::make_shared<Packet>(*previewPacket);
                    smokeFiltered = compressed;
                    debugTab(true);
                    find<ComboBox>(L"DebugLeft").SelectedIndex(0);
                    find<ComboBox>(L"DebugRight").SelectedIndex(12);
                    smokeStage = 2;
                } else if (r.kind == Result::Debug && smokeStage == 2) {
                    smokeDebugCachedData=debugCache?debugCache->final.pixels.data():nullptr;
                    log << "debug image and metadata "
                        << (debugFrame && find<TextBox>(L"DebugReport").Text().size() > 100 ? "PASS" : "ERROR") << '\n';
                    zoom(2.);
                    const double dpi = root.XamlRoot() ? root.XamlRoot().RasterizationScale() : 1;
                    log << "debug shared zoom "
                        << (std::abs(find<Grid>(L"Surface").Width() - debugFrame->width / dpi * 2) < 1 ? "PASS"
                                                                                                       : "ERROR")
                        << '\n';
                    log << "independent right debug panel "
                        << (find<UIElement>(L"Sidebar").Visibility() == Visibility::Visible &&
                                    find<UIElement>(L"CompressionControls").Visibility() == Visibility::Visible &&
                                    find<UIElement>(L"DebugSidebar").Visibility() == Visibility::Visible &&
                                    find<Grid>(L"Workspace").ColumnDefinitions().GetAt(2).Width().Value > 0 &&
                                    find<Slider>(L"Quality").IsEnabled()
                                ? "PASS"
                                : "ERROR")
                        << '\n';
                    auto view = find<ScrollViewer>(L"Viewport");
                    auto surface = find<Grid>(L"Surface");
                    const winrt::Windows::Foundation::Point anchor{float(view.ViewportWidth() * .37),
                                                                   float(view.ViewportHeight() * .43)};
                    const auto beforeOrigin = surface.TransformToVisual(view).TransformPoint({0, 0});
                    const double imageX = (anchor.X - beforeOrigin.X) / viewScale,
                                 imageY = (anchor.Y - beforeOrigin.Y) / viewScale;
                    smokeZoomX = imageX;
                    smokeZoomY = imageY;
                    smokeZoomAnchor = anchor;
                    zoomAt(viewScale * 1.4, anchor);
                    log << "debug source unchanged "
                        << (compressed == smokeFiltered && previewPacket->data == smokePacket->data ? "PASS" : "ERROR")
                        << '\n';
                    // No manual Analyze call: both selection and gain must refresh automatically.
                    find<ComboBox>(L"DebugRight").SelectedIndex(7);
                    find<Slider>(L"DebugGain").Value(2);
                    smokeStage = 4;
                } else if (r.kind == Result::Debug && smokeStage == 4) {
                    log << "debug view reuses validated decode "
                        << (smokeDebugCachedData&&debugCache&&debugCache->final.pixels.data()==smokeDebugCachedData?"PASS":"ERROR") << '\n';
                    // ScrollViewer applies ChangeView on the next layout/render pass.
                    auto view = find<ScrollViewer>(L"Viewport");
                    auto surface = find<Grid>(L"Surface");
                    const auto origin = surface.TransformToVisual(view).TransformPoint({0, 0});
                    log << "cursor anchored zoom "
                        << (std::abs(smokeZoomX * viewScale + origin.X - smokeZoomAnchor.X) < 2 &&
                                    std::abs(smokeZoomY * viewScale + origin.Y - smokeZoomAnchor.Y) < 2
                                ? "PASS"
                                : "ERROR")
                        << " dx=" << smokeZoomX * viewScale + origin.X - smokeZoomAnchor.X
                        << " dy=" << smokeZoomY * viewScale + origin.Y - smokeZoomAnchor.Y << '\n';
                    log << "automatic selection and gain refresh "
                        << (debugFrame &&
                                    std::wstring(find<TextBlock>(L"DebugCaption").Text()).find(debugViewNames[7]) !=
                                        std::wstring::npos
                                ? "PASS"
                                : "ERROR")
                        << '\n';
                    fitMode = true;
                    fit();
                    view.UpdateLayout();
                    const auto fitOrigin = surface.TransformToVisual(view).TransformPoint({0, 0});
                    smokeZoomAnchor = {float(fitOrigin.X + surface.Width() * .65),
                                       float(fitOrigin.Y + surface.Height() * .4)};
                    smokeZoomX = surface.Width() * .65 / viewScale;
                    smokeZoomY = surface.Height() * .4 / viewScale;
                    zoomAt(1., smokeZoomAnchor);
                    find<Slider>(L"DebugGain").Value(3);
                    smokeStage = 5;
                } else if (r.kind == Result::Debug && smokeStage == 5) {
                    const auto origin = find<Grid>(L"Surface")
                                            .TransformToVisual(find<ScrollViewer>(L"Viewport"))
                                            .TransformPoint({0, 0});
                    log << "letterbox cursor zoom "
                        << (std::abs(smokeZoomX * viewScale + origin.X - smokeZoomAnchor.X) < 2 &&
                                    std::abs(smokeZoomY * viewScale + origin.Y - smokeZoomAnchor.Y) < 2
                                ? "PASS"
                                : "ERROR")
                        << " dx=" << smokeZoomX * viewScale + origin.X - smokeZoomAnchor.X
                        << " dy=" << smokeZoomY * viewScale + origin.Y - smokeZoomAnchor.Y << '\n';
                    debugTab(false);
                    log << "debug disabled releases image "
                        << (!debugFrame && !debugBitmap && !debugCache && !find<ToggleSwitch>(L"HeatmapToggle").IsOn() ? "PASS"
                                                                                                        : "ERROR")
                        << '\n';
                    debugTab(true);
                    find<ComboBox>(L"DebugLeft").SelectedIndex(16);
                    find<ComboBox>(L"DebugRight").SelectedIndex(17);
                    smokeStage = 3;
                } else if (r.kind == Result::Debug && smokeStage == 3) {
                    log << "actual AQ maps "
                        << (budgetMap && budgetFrame && budgetMap->penalties[0].size() == budgetMap->bits.size()
                                ? "PASS"
                                : "ERROR")
                        << '\n';
                    log << "view help has current packet prices "
                        << (!debugCosts[2].empty() && !debugCosts[18].empty() ? "PASS" : "ERROR") << '\n';
                    const int selected = find<ComboBox>(L"DebugRight").SelectedIndex();
                    cycleDebugView(1, -120);
                    log << "wheel changes debug view "
                        << (find<ComboBox>(L"DebugRight").SelectedIndex() == selected + 1 ? "PASS" : "ERROR") << '\n';
                    smokeBudget = budgetMap;
                    smokeFlatAllocation = uint32_t(find<Slider>(L"FlatAllocation").Value());
                    find<ComboBox>(L"DebugLeft").SelectedIndex(20);
                    find<ComboBox>(L"DebugRight").SelectedIndex(0);
                    find<Slider>(L"FlatAllocation").Value(100);
                    smokeStage = 7;
                } else if (r.kind == Result::Debug && smokeStage == 7) {
                    log << "automatic flat savings comparison "
                        << (!allocationSavings.empty() &&
                            std::wstring(find<TextBlock>(L"FlatSavingsReport").Text()).find(L"Celý soubor") != std::wstring::npos
                                ? "PASS" : "ERROR") << '\n';
                    log << "automatic allocation encode and map refresh "
                        << (previewOptions && previewOptions->flatAllocation == 100 && budgetMap && smokeBudget &&
                                    budgetMap->flatCoverage != smokeBudget->flatCoverage &&
                                    previewPacket->data != smokePacket->data
                                ? "PASS"
                                : "ERROR")
                        << '\n';
                    find<Slider>(L"FlatAllocation").Value(smokeFlatAllocation);
                    smokeStage = 8;
                } else if (r.kind == Result::Debug && smokeStage == 8) {
                    log << "allocation baseline restores encoded bytes "
                        << (previewPacket->data == smokePacket->data ? "PASS" : "ERROR") << '\n';
                    smokeBudget=budgetMap;
                    smokeR441=encodeOptions().reconstruction441;
                    find<Slider>(L"R441Antialias").Value(smokeR441.antialias==50?25:50);
                    smokeStage=9;
                } else if (r.kind == Result::Debug && smokeStage == 9) {
                    log << "debug filter edit reuses allocation map "
                        << (budgetMap==smokeBudget&&previewOptions&&previewOptions->reconstruction441!=smokeR441?"PASS":"ERROR") << '\n';
                    find<Slider>(L"R441Antialias").Value(smokeR441.antialias);
                    smokeStage=10;
                } else if (r.kind == Result::Debug && smokeStage == 10) {
                    log << "debug filter restore preserves packet "
                        << (budgetMap==smokeBudget&&previewPacket->data==smokePacket->data?"PASS":"ERROR") << '\n';
                    find<CheckBox>(L"AdaptiveQuantizationEnabled").IsChecked(false);
                    smokeStage = 11;
                } else if (r.kind == Result::Debug && smokeStage == 11) {
                    bool neutral = previewOptions && !previewOptions->adaptiveQuantizationEnabled && budgetMap;
                    if (budgetMap) for (const auto& weights : budgetMap->penalties)
                        for (float weight : weights) neutral = neutral && weight == 1;
                    log << "AQ off refreshes neutral weights and disables contrast slider "
                        << (neutral && !find<Slider>(L"FlatAllocation").IsEnabled() &&
                            !find<Slider>(L"FlatSavings").IsEnabled() ? "PASS" : "ERROR") << '\n';
                    find<CheckBox>(L"AdaptiveQuantizationEnabled").IsChecked(true);
                    smokeStage = 12;
                } else if (r.kind == Result::Debug && smokeStage == 12) {
                    log << "AQ on restores packet and contrast control "
                        << (previewOptions && previewOptions->adaptiveQuantizationEnabled &&
                            previewPacket->data == smokePacket->data && find<Slider>(L"FlatAllocation").IsEnabled()
                            ? "PASS" : "ERROR") << '\n';
                    smokeFlatSavings = uint32_t(find<Slider>(L"FlatSavings").Value());
                    find<Slider>(L"FlatSavings").Value(smokeFlatSavings == 100 ? 0 : 100);
                    smokeStage = 13;
                } else if (r.kind == Result::Debug && smokeStage == 13) {
                    log << "extra flat savings automatically reencodes "
                        << (previewOptions && previewOptions->flatSavings != smokeFlatSavings &&
                            previewPacket->data != smokePacket->data && !allocationSavings.empty() ? "PASS" : "ERROR") << '\n';
                    find<Slider>(L"FlatSavings").Value(smokeFlatSavings);
                    smokeStage = 14;
                } else if (r.kind == Result::Debug && smokeStage == 14) {
                    log << "extra flat savings baseline restores packet "
                        << (previewOptions && previewOptions->flatSavings == smokeFlatSavings &&
                            previewPacket->data == smokePacket->data ? "PASS" : "ERROR") << '\n';
                    smokeFlatThreshold=uint32_t(find<Slider>(L"FlatThreshold").Value());
                    find<Slider>(L"FlatThreshold").Value(smokeFlatThreshold==100?0:100);
                    smokeStage=15;
                } else if(r.kind==Result::Debug && smokeStage==15) {
                    log << "minimum-data threshold automatically reencodes "
                        << (previewOptions && previewOptions->flatThreshold!=smokeFlatThreshold &&
                            previewPacket->data!=smokePacket->data?"PASS":"ERROR") << '\n';
                    find<Slider>(L"FlatThreshold").Value(smokeFlatThreshold);
                    smokeStage=16;
                } else if(r.kind==Result::Debug && smokeStage==16) {
                    log << "threshold baseline restores packet "
                        << (previewPacket->data==smokePacket->data?"PASS":"ERROR") << '\n';
                    smokeBudget.reset();
                    find<Expander>(L"ContourControls").IsExpanded(false);
                    find<Expander>(L"AllocationControls").IsExpanded(true);
                    fitMode = true;
                    fit();
                    smokeStage = 90;
                    smokeStart = std::chrono::steady_clock::now();
                }
                return;
            }
            if (smoke) {
                std::ofstream log(executableDirectory() / L"ui-smoke.txt", std::ios::app);
                log << "final sharpening controls visibility "
                    << ((find<Expander>(L"LegacyFilter").Visibility() == (sharpFilter?Visibility::Visible:Visibility::Collapsed) &&
                         find<StackPanel>(L"SmoothingPanel").Visibility() == Visibility::Collapsed &&
                         find<StackPanel>(L"SharpnessPanel").Visibility() == (sharpFilter?Visibility::Visible:Visibility::Collapsed))
                            ? "PASS"
                            : "ERROR")
                    << "\n";
                if(r.error.empty()&&r.kind==Result::Preview&&smokeStage==70){
                    log<<"speed mode switch recomputes packet "
                       <<(!r.reusedCompression&&previewPacket&&previewPacket->data!=smokeSpeedPacket->data&&
                           compressed->pixels!=smokeSpeedFrame->pixels?"PASS":"ERROR")<<'\n';
                    find<ComboBox>(L"EncodeSpeed").SelectedIndex(smokeSavedSpeed);smokeStage=71;preview();return;
                }
                if(r.error.empty()&&r.kind==Result::Preview&&smokeStage==71){
                    log<<"speed mode restore exact packet and pixels "
                       <<(!r.reusedCompression&&previewPacket&&previewPacket->data==smokeSpeedPacket->data&&
                           compressed->pixels==smokeSpeedFrame->pixels?"PASS":"ERROR")<<'\n';
                    smokeSpeedChecked=true;smokeStage=1;smokeSpeedPacket.reset();smokeSpeedFrame.reset();
                }
                if(r.error.empty()&&r.kind==Result::Preview&&smokeStage==1&&!smokeSpeedChecked&&sourceInfo.image){
                    smokeSavedSpeed=find<ComboBox>(L"EncodeSpeed").SelectedIndex();
                    smokeSpeedPacket=previewPacket;smokeSpeedFrame=compressed;
                    find<ComboBox>(L"EncodeSpeed").SelectedIndex(smokeSavedSpeed==1?0:1);smokeStage=70;preview();return;
                }
                if (smokeStage == 1 && r.kind == Result::Preview) {
                    auto versions = find<ComboBox>(L"CodecVersion");
                    const auto saved = versions.SelectedIndex();
                    bool valid = versions.Items().Size() == encoderCatalog.size();
                    for (size_t i = 0; i < encoderCatalog.size(); ++i) {
                        versions.SelectedIndex(int(i));
                        valid = valid && encodeOptions().revision == encoderCatalog[i].revision && encodeOptions().backend==encoderCatalog[i].backend &&decoderBackend==encoderCatalog[i].backend &&
                                versions.Items()
                                        .GetAt(uint32_t(i))
                                        .as<ComboBoxItem>()
                                        .Content()
                                        .as<IPropertyValue>()
                                        .GetString() == encoderCatalog[i].name &&
                                (find<TextBlock>(L"EncoderSummary").Text() == encoderCatalog[i].summary);
                    }
                    versions.SelectedIndex(saved);
                    log<<"unified CPU/GPU encoder and decoder binding "<<(valid?"PASS":"ERROR")<<'\n';
                    auto speedMode=find<ComboBox>(L"EncodeSpeed");const auto savedSpeed=speedMode.SelectedIndex();
                    speedMode.SelectedIndex(1);const bool fastBound=encodeOptions().speed==EncodeSpeed::Fast;
                    speedMode.SelectedIndex(0);const bool qualityBound=encodeOptions().speed==EncodeSpeed::MoreQuality;
                    speedMode.SelectedIndex(2);const bool maxBound=encodeOptions().speed==EncodeSpeed::MaxCompression;
                    speedMode.SelectedIndex(savedSpeed);
                    log<<"independent Fast / More quality binding "<<(fastBound&&qualityBound?"PASS":"ERROR")<<'\n';
                    log<<"Fast-based Max compression binding "<<(maxBound?"PASS":"ERROR")<<'\n';
                    auto temporal=find<ToggleSwitch>(L"Temporal");auto interval=find<NumberBox>(L"KeyframeInterval");
                    const auto savedTemporal=temporal.IsOn();const double savedInterval=interval.Value();
                    temporal.IsOn(true);interval.Value(0);saveSettings();
                    const bool temporalSaved=GetPrivateProfileIntW(L"Studio",L"temporal",0,settingsPath().c_str())==1&&GetPrivateProfileIntW(L"Studio",L"keyframeInterval",1,settingsPath().c_str())==0;
                    interval.Value(300);saveSettings();
                    const bool intervalSaved=GetPrivateProfileIntW(L"Studio",L"keyframeInterval",0,settingsPath().c_str())==300;
                    temporal.IsOn(savedTemporal);interval.Value(savedInterval);saveSettings();
                    log<<"temporal toggle / adaptive keyframe interval persistence "<<(temporalSaved&&intervalSaved?"PASS":"ERROR")<<'\n';
                    auto method=find<ComboBox>(L"TemporalMethod");const auto savedMethod=method.SelectedIndex();
                    method.SelectedIndex(0);saveSettings();const bool coefficientsSaved=GetPrivateProfileIntW(L"Studio",L"coefficientTemporal",0,settingsPath().c_str())==1;
                    method.SelectedIndex(1);saveSettings();const bool regionsSaved=GetPrivateProfileIntW(L"Studio",L"coefficientTemporal",1,settingsPath().c_str())==0;
                    method.SelectedIndex(2);saveSettings();const bool maximumSaved=GetPrivateProfileIntW(L"Studio",L"temporalMethod",0,settingsPath().c_str())==2;
                    method.SelectedIndex(savedMethod);saveSettings();
                    log<<"coefficient / similar-region / maximum temporal mode persistence "<<(coefficientsSaved&&regionsSaved&&maximumSaved?"PASS":"ERROR")<<'\n';
                    auto tolerance=find<NumberBox>(L"SimilarTolerance"),hold=find<NumberBox>(L"SimilarHold");
                    const double savedTolerance=tolerance.Value(),savedHold=hold.Value();
                    tolerance.Value(3);hold.Value(10);saveSettings();
                    const bool reuseSaved=GetPrivateProfileIntW(L"Studio",L"similarTolerance",0,settingsPath().c_str())==3&&GetPrivateProfileIntW(L"Studio",L"similarHold",0,settingsPath().c_str())==10;
                    tolerance.Value(savedTolerance);hold.Value(savedHold);saveSettings();
                    log<<"similar-region tolerance / hold persistence "<<(reuseSaved?"PASS":"ERROR")<<'\n';
                    auto maxTolerance=find<NumberBox>(L"MaxTolerance");const double savedMaxTolerance=maxTolerance.Value();
                    maxTolerance.Value(8);method.SelectedIndex(2);saveSettings();
                    const bool separateTolerance=GetPrivateProfileIntW(L"Studio",L"maxTolerance",0,settingsPath().c_str())==8&&tolerance.Value()==savedTolerance;
                    maxTolerance.Value(savedMaxTolerance);method.SelectedIndex(savedMethod);saveSettings();
                    log<<"separate maximum / region tolerance settings "<<(separateTolerance?"PASS":"ERROR")<<'\n';
                    auto allocation = find<Slider>(L"FlatAllocation");
                    const auto savedAllocation = allocation.Value();
                    const auto targetAllocation=savedAllocation==100?0:100;
                    allocation.Value(targetAllocation);
                    auto selectedOptions = encodeOptions();
                    log << "single allocation control binding "
                        << (selectedOptions.flatAllocation == uint32_t(targetAllocation) &&
                                    !samePreviewCompression(selectedOptions, *previewOptions)
                                ? "PASS"
                                : "ERROR")
                        << "\n";
                    allocation.Value(savedAllocation);
                    if (smokeHeatmap) {
                        const bool validMap = budgetFrame && budgetFrame->pixels.size() &&
                                              find<Image>(L"BudgetOverlay").Visibility() == Visibility::Visible;
                        log << "heatmap overlay " << (validMap ? "PASS" : "ERROR") << "\n";
                        if (validMap) {
                            writePng(executableDirectory() / L"ui-budget-map.png", *budgetFrame);
                        }
                    }
                    log << "version selector binding " << (valid ? "PASS" : "ERROR") << "\n";
                    log << "encoder catalog descriptions " << (valid ? "PASS" : "ERROR") << "\n";
                    encoderHelp();
                }
                log << "stage " << smokeStage << " error=" << to_string(r.error) << "\n";
                if(r.error.empty()&&r.kind==Result::Preview&&r.encodedOptions&&previewPacket&&original){
                    const auto stored=read441(previewPacket->data,original->width,original->height);
                    const auto expected=L"Použito: "+number(100*skirtBlend(stored.settings,cleanupQuality(stored.budget,original->width,original->height)),1)+L" %";
                    log<<"Skirt actual value from encoded packet "
                       <<(std::wstring(find<TextBlock>(L"R441ActualValue").Text())==expected?"PASS":"ERROR")<<'\n';
                }
                if (r.error.empty() && smokeStage == 0) {
                    if (r.kind == Result::Sequence)
                        smokeStage = 10;
                    else {
                        ++smokeStage;
                        preview();
                    }
                } else if (r.error.empty() && r.kind == Result::Preview && smokeStage == 1) {
                    if (!legacyFilter) {
                        log << "independent transform preview PASS\n";
                        showSizeChange(9100000, 30000, L"test");
                        auto decrease = find<TextBlock>(L"SizeChange").Text();
                        auto green = find<TextBlock>(L"SizeChange").Foreground().as<SolidColorBrush>().Color();
                        log << "size decrease 99.67% "
                            << (std::wstring(decrease.c_str()).find(L"99.67") != std::wstring::npos && green.G > green.R
                                    ? "PASS"
                                    : "ERROR")
                            << "\n";
                        showSizeChange(30000, 60000, L"test");
                        auto increase = find<TextBlock>(L"SizeChange").Text();
                        auto red = find<TextBlock>(L"SizeChange").Foreground().as<SolidColorBrush>().Color();
                        log << "size increase 100% "
                            << (std::wstring(increase.c_str()).find(L"100.00") != std::wstring::npos && red.R > red.G
                                    ? "PASS"
                                    : "ERROR")
                            << "\n";
                        if (sourceInfo.image) {
                            smokePacket = previewPacket;
                            if (true) {
                                smokeR441 = encodeOptions().reconstruction441;
                                smokeR441Intensity=uint32_t(find<Slider>(L"R441Intensity").Value());
                                smokeFiltered = compressed;
                                smokeSurfaces = encodeOptions().contourIntensity;
                                smokeContours = encodeOptions().shapeIntensity;
                                find<Slider>(L"ShapeIntensity").Value(smokeContours == 200 ? 0 : 200);
                                smokeStage = 50;
                                preview();
                            } else {
                                smokeStage = 40;
                                auto folder = executableDirectory().parent_path() / L"smoke-artifacts";
                                std::filesystem::create_directories(folder);
                                convertTo(folder /
                                          (L"ui-cache_q" + std::to_wstring(int(encodeOptions().quality)) + L".fajk"));
                            }
                        } else {
                            showSizeChange(original->pixels.size(), r.size,
                                           L"Jeden snímek · RGBA → komprimovaný obraz");
                            smokeStage = 2;
                        }
                    } else {
                        smokeDeblocking = deblocking;
                        smokePacket = previewPacket;
                        find<Slider>(L"Deblocking").Value(0);
                        smokeStage = 30;
                        applyDeblocking();
                    }
                } else if (r.error.empty() && r.kind == Result::Preview && smokeStage == 50) {
                    const auto offset =
                        previewPacket->data[4] == reconstruction441Version ? reconstruction441HeaderBytes : 0u;
                    const auto savedControls =
                        previewPacket->data[offset + 5] >= 6 && previewPacket->data[offset + 22] ? 8u : 0u;
                    const bool sameChild =
                        previewPacket->data.size() == smokePacket->data.size() &&
                        std::equal(previewPacket->data.begin() + offset + 28, previewPacket->data.end() - savedControls,
                                   smokePacket->data.begin() + offset + 28);
                    log << "contour check: options=" << bool(r.encodedOptions && r.encodedOptions->shapeIntensity != smokeContours && r.encodedOptions->contourIntensity == smokeSurfaces)
                        << " coefficients=" << sameChild << " pixelsChanged=" << (compressed->pixels != smokeFiltered->pixels) << "\n";
                    // Some images do not change pixels under this filter. Verify
                    // the stored controls and new preview rather than requiring
                    // every arbitrary source to exhibit a visible contour change.
                    const uint32_t savedContour=uint32_t(previewPacket->data[offset+20])|(uint32_t(previewPacket->data[offset+21])<<8);
                    log << "Reconstruction contour slider updates saved controls and preview "
                        << (r.encodedOptions && r.encodedOptions->shapeIntensity != smokeContours &&
                                    r.encodedOptions->contourIntensity == smokeSurfaces && sameChild &&
                                    savedContour==r.encodedOptions->shapeIntensity && previewPacket->data!=smokePacket->data
                                ? "PASS"
                                : "ERROR")
                        << "\n";
                    find<Slider>(L"ContourIntensity").Value(smokeSurfaces ? 0 : 200);
                    smokeStage = 51;
                    preview();
                } else if (r.error.empty() && r.kind == Result::Preview && smokeStage == 51) {
                    log << "Reconstruction surface slider invalidates preview "
                        << (r.encodedOptions && r.encodedOptions->contourIntensity != smokeSurfaces ? "PASS" : "ERROR")
                        << "\n";
                    find<Slider>(L"ContourIntensity").Value(smokeSurfaces);
                    find<Slider>(L"ShapeIntensity").Value(smokeContours);
                    smokeStage = 52;
                    preview();
                } else if (r.error.empty() && r.kind == Result::Preview && smokeStage == 52) {
                    log << "Reconstruction slider restore exact packet and pixels "
                        << (previewPacket->data == smokePacket->data && compressed->pixels == smokeFiltered->pixels
                                ? "PASS"
                                : "ERROR")
                        << "\n";
                    log << "compact encoder help "
                        << (find<Button>(L"EncoderHelp").Width() == 32 &&
                                    find<TextBlock>(L"EncoderSummary").Visibility() == Visibility::Collapsed
                                ? "PASS"
                                : "ERROR")
                        << "\n";
                    if (encodeOptions().revision == EncoderRevision::Experimental011441R441) {
                        smokeStage = 60;
                        find<Slider>(L"StrongerTangents").Value(smokeR441.strongerTangents == 100 ? 0 : 100);
                        preview();
                    } else {
                        smokeFiltered.reset();
                        smokeStage = 40;
                        auto folder = executableDirectory().parent_path() / L"smoke-artifacts";
                        std::filesystem::create_directories(folder);
                        convertTo(folder / (L"ui-cache_q" + std::to_wstring(int(encodeOptions().quality)) + L".fajk"));
                    }
                } else if (r.error.empty() && r.kind == Result::Preview && smokeStage >= 60 && smokeStage <= 66) {
                    const auto current = encodeOptions().reconstruction441;
                    const bool sameBase =
                        previewPacket->data.size() == smokePacket->data.size() &&
                        std::equal(previewPacket->data.begin() + reconstruction441HeaderBytes,
                                   previewPacket->data.end(), smokePacket->data.begin() + reconstruction441HeaderBytes);
                    bool changed = false;
                    const char* name = "";
                    if (smokeStage == 60) {
                        name = "Stronger Tangents";
                        changed = current.strongerTangents != smokeR441.strongerTangents;
                    }
                    if (smokeStage == 61) {
                        name = "antialias";
                        changed = current.antialias != smokeR441.antialias;
                    }
                    if (smokeStage == 62) {
                        name = "contrast";
                        changed = current.contrast != smokeR441.contrast;
                    }
                    if (smokeStage == 63) {
                        name = "intensity";
                        changed = current.intensity != smokeR441.intensity;
                        if(!current.fixedIntensity&&encodeOptions().quality>=50)
                            changed=current.intensity==0&&find<Slider>(L"R441Intensity").Value()!=smokeR441Intensity;
                    }
                    if (smokeStage == 64) {
                        name = "text protection";
                        changed = current.protectText != smokeR441.protectText;
                    }
                    if (smokeStage == 65) {
                        name = "style";
                        changed = current.style != smokeR441.style;
                    }
                    if (smokeStage < 66)
                        log << "R441 " << name << " binding + reused compressed base "
                            << (changed && r.reusedCompression && sameBase ? "PASS" : "ERROR") << "\n";
                    if (smokeStage == 60) {
                        find<Slider>(L"StrongerTangents").Value(smokeR441.strongerTangents);
                        find<Slider>(L"R441Antialias").Value(smokeR441.antialias == 100 ? 0 : 100);
                    }
                    if (smokeStage == 61) {
                        find<Slider>(L"R441Antialias").Value(smokeR441.antialias);
                        find<Slider>(L"R441Contrast").Value(smokeR441.contrast == 70 ? 0 : 70);
                    }
                    if (smokeStage == 62) {
                        find<Slider>(L"R441Contrast").Value(smokeR441.contrast);
                        find<Slider>(L"R441Intensity").Value(smokeR441Intensity ? 0 : 100);
                    }
                    if (smokeStage == 63) {
                        find<Slider>(L"R441Intensity").Value(smokeR441Intensity);
                        find<CheckBox>(L"R441ProtectText").IsChecked(!smokeR441.protectText);
                    }
                    if (smokeStage == 64) {
                        find<CheckBox>(L"R441ProtectText").IsChecked(smokeR441.protectText);
                        find<ComboBox>(L"R441Style").SelectedIndex(smokeR441.style ? 0 : 1);
                    }
                    if (smokeStage == 65)
                        find<ComboBox>(L"R441Style").SelectedIndex(smokeR441.style);
                    if (smokeStage == 66) {
                        log << "R441 restore exact saved controls, packet and pixels "
                            << (current == smokeR441 && previewPacket->data == smokePacket->data &&
                                        compressed->pixels == smokeFiltered->pixels
                                    ? "PASS"
                                    : "ERROR")
                            << "\n";
                        smokeFiltered.reset();
                        smokeStage = 40;
                        auto folder = executableDirectory().parent_path() / L"smoke-artifacts";
                        std::filesystem::create_directories(folder);
                        convertTo(folder / (L"ui-cache_q" + std::to_wstring(int(encodeOptions().quality)) + L".fajk"));
                    } else {
                        ++smokeStage;
                        preview();
                    }
                } else if (r.error.empty() && r.kind == Result::Converted && smokeStage == 40) {
                    SequenceReader check(sequencePath);
                    log << "cached preview save "
                        << (check.info().count == 1 && check.read(0).data == smokePacket->data ? "PASS" : "ERROR")
                        << "\n";
                    smokePacket.reset();
                    smokeStage = 41;
                } else if (r.error.empty() && r.kind == Result::Preview && smokeStage == 30) {
                    log << "deblocking off same packet "
                        << (deblocking == 0 && previewPacket == smokePacket ? "PASS" : "ERROR") << "\n";
                    find<Slider>(L"Deblocking").Value(smokeDeblocking);
                    smokeStage = 31;
                    applyDeblocking();
                } else if (r.error.empty() && r.kind == Result::Preview && smokeStage == 31) {
                    log << "deblocking restore same packet "
                        << (deblocking == smokeDeblocking && previewPacket == smokePacket ? "PASS" : "ERROR") << "\n";
                    smokePacket.reset();
                    smokeStage = 2;
                } else {
                    ++smokeStage;
                }
            }
        }
        const int64_t now = playing ? playbackTime() : 0;
        auto frame=decoder?decoder->takeDue(generation,playing,now,smoke&&smokeStage==12?&smokeSkipped:nullptr):std::optional<DecodeResult>{};
        if (frame && frame->generation == generation) {
            if (!frame->error.empty()) {
                stopPlay();
                requested = UINT64_MAX;
                error(frame->error);
            } else if (playing || frame->frame == requested) {
                if (smoke && smokeStage == 12)
                    ++smokePresented;
                auto displayStart = std::chrono::steady_clock::now();
                invalidateDebug();
                current = frame->frame;
                presentedFilter = frame->filter;
                presentedSharpening = frame->sharpening;
                if (frame->reference)
                    original = frame->reference;
                else if (sourcePath.empty() || !sourceInfo.image)
                    original.reset();
                if (sequencePath.empty()) {
                    compressed.reset();
                    previewPacket.reset();
                    compressedUploaded.reset();
                    setMode(0);
                } else {
                    compressed = find<ComboBox>(L"ViewMode").SelectedIndex() == 0 ? nullptr : frame->image;
                    render();
                }
                if (smoke && smokeStage == 12)
                    smokeUploadMs +=
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - displayStart)
                            .count();
                settingTimeline = true;
                find<Slider>(L"Timeline").Value(double(current));
                settingTimeline = false;
                status(L"Snímek " + std::to_wstring(current + 1) + L" / " + (sequencePath.empty() ? L"≈ " : L"") +
                       std::to_wstring(lastFrame() + 1) +
                       (frame->throughputMs>0&&frame->throughputFrames>=4?
                            L" · decode průměr " + number(frame->throughputMs) + L" ms/snímek (" + number(1000/frame->throughputMs) + L" snímků/s)":
                            (frame->lanes>1?std::wstring(L" · decode průměr: měřím…"):std::wstring{})) +
                       (frame->decodeMs >= 0 ? L" · latence úlohy " + number(frame->decodeMs) + L" ms" : L"") +
                       (frame->lanes>1?L" · převzetí z fronty ":L" · příprava snímku ") + number(frame->ms) + L" ms");
            }
        }
        if (playing && now >= duration()) {
            stopPlay();
            requestFrame(lastFrame());
        }
        if (smokeDebug)
            return;
        if (smoke && smokeStage == 10 && shown && !index.empty()) {
            smokeStage = 11;
            find<ComboBox>(L"Step").SelectedIndex(2);
            step(1);
        } else if (smoke && smokeStage == 11 && current == std::min<uint64_t>(4, index.size() - 1)) {
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app) << "seek stride 4 PASS\n";
            find<ComboBox>(L"Step").SelectedIndex(0);
            smokeStage = 12;
            smokeStart = std::chrono::steady_clock::now();
            play();
        }
        if (smoke && smokeStage == 12 && std::chrono::steady_clock::now() - smokeStart > std::chrono::seconds(3)) {
            const std::wstring timingStatus{find<TextBlock>(L"Status").Text()};
            const bool measured=timingStatus.find(L"decode průměr ")!=std::wstring::npos&&
                timingStatus.find(L"ms/snímek")!=std::wstring::npos&&timingStatus.find(L"snímků/s")!=std::wstring::npos&&
                timingStatus.find(L"latence úlohy")!=std::wstring::npos;
            std::ofstream(executableDirectory()/L"ui-smoke.txt",std::ios::app)
                <<"measured playback throughput status "<<(measured?"PASS":"ERROR")<<'\n';
            stopPlay();
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app)
                << "playback 3s presented=" << smokePresented << " skipped=" << smokeSkipped << " last=" << current
                << " upload_ms=" << smokeUploadMs / std::max<uint64_t>(1, smokePresented) << "\n";
            smokeStage = 2;
        }
        if (smoke && smokeStage == 41 && compressed && !busy) {
            setMode(2);
            writePng(executableDirectory().parent_path() / L"smoke-artifacts/ui-export.png", *compressed);
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app)
                << "saved image decode and PNG export PASS\n";
            if (colorFilter) {
                smokeFiltered = compressed;
                smokeDeblocking = deblocking;
                smokeTargetFilter = deblocking ? 0 : 100;
                find<Slider>(L"Deblocking").Value(smokeTargetFilter);
                smokeStage = 43;
            } else
                beginSharpnessSmoke();
        }
        if (smoke && smokeStage == 43 && !filterPending && compressed && presentedFilter == smokeTargetFilter) {
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app)
                << "live filter change " << (compressed->pixels != smokeFiltered->pixels ? "PASS" : "ERROR") << "\n";
            find<Slider>(L"Deblocking").Value(smokeDeblocking);
            smokeStage = 44;
        }
        if (smoke && smokeStage == 44 && !filterPending && compressed && presentedFilter == smokeDeblocking) {
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app)
                << "live filter restore " << (compressed->pixels == smokeFiltered->pixels ? "PASS" : "ERROR") << "\n";
            smokeFiltered.reset();
            beginSharpnessSmoke();
        }
        if (smoke && smokeStage == 45 && !filterPending && compressed && presentedSharpening == smokeTargetSharp) {
            SequenceReader reader(sequencePath);
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app)
                << "live sharpening change same packet "
                << (compressed->pixels != smokeFiltered->pixels && reader.read(0).data == smokePacket->data ? "PASS"
                                                                                                            : "ERROR")
                << "\n";
            find<Slider>(L"Sharpening").Value(smokeSharpening);
            smokeStage = 46;
        }
        if (smoke && smokeStage == 46 && !filterPending && compressed && presentedSharpening == smokeSharpening) {
            std::ofstream(executableDirectory() / L"ui-smoke.txt", std::ios::app)
                << "live sharpening restore " << (compressed->pixels == smokeFiltered->pixels ? "PASS" : "ERROR")
                << "\n";
            smokeFiltered.reset();
            smokePacket.reset();
            find<Expander>(L"LegacyFilter").IsExpanded(true);
            smokeStage = 2;
        }
        if (smoke && smokeStage == 2 && !busy) {
            smokeStage = 20;
            smokeStart = std::chrono::steady_clock::now();
            zoom(2.);
            find<Slider>(L"Split").Value(27);
        } else if (smoke && smokeStage == 20 &&
                   std::chrono::steady_clock::now() - smokeStart > std::chrono::milliseconds(500)) {
            std::ofstream log(executableDirectory() / L"ui-smoke.txt", std::ios::app);
            log << "zoom 200% "
                << (std::abs(find<Grid>(L"Surface").Width() -
                             shown->width / (root.XamlRoot() ? root.XamlRoot().RasterizationScale() : 1) * 2) < 1.
                        ? "PASS"
                        : "ERROR")
                << "\n";
            if (original && compressed) {
                auto clip = find<Image>(L"Reference").Clip().as<RectangleGeometry>();
                log << "comparison 27% "
                    << (std::abs(clip.Rect().Width - find<Grid>(L"Surface").Width() * .27) < 1. ? "PASS" : "ERROR")
                    << "\n";
            }
            fitMode = true;
            fit();
            smokeStage = 21;
        } else if (smoke && smokeStage == 21) {
            smokeStage = 3;
            captureSmoke();
        }
    }
    void OnLaunched(LaunchActivatedEventArgs const&) {
        try {
            std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app) << "launched\n";
            Resources().MergedDictionaries().Append(XamlControlsResources());
            std::ifstream file(executableDirectory() / L"studio.xaml", std::ios::binary);
            std::string markup((std::istreambuf_iterator<char>(file)), {});
            fajk::require(!markup.empty(), "studio.xaml is missing.");
            root = Markup::XamlReader::Load(to_hstring(markup)).as<FrameworkElement>();
            std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app) << "xaml loaded\n";
            for (auto name : {L"Error", L"Progress", L"Cancel", L"Empty", L"Status"}) {
                auto value = root.FindName(name);
                fajk::require(bool(value), "XAML control missing.");
                named.emplace(name, value);
            }
            // Adopt the explicitly approved R460 controls once, before loading sliders.
            // Keep unrelated settings and future user edits intact.
            const auto defaultsPath = settingsPath();
            if (GetPrivateProfileIntW(L"Studio", L"cleanupDefaultsVersion", 0, defaultsPath.c_str()) < 2) {
                WritePrivateProfileStringW(L"Studio", L"r441Intensity", L"50", defaultsPath.c_str());
                WritePrivateProfileStringW(L"Studio", L"r441Tangents", L"0", defaultsPath.c_str());
                WritePrivateProfileStringW(L"Studio", L"shapeIntensity", L"200", defaultsPath.c_str());
                WritePrivateProfileStringW(L"Studio", L"contourIntensity", L"0", defaultsPath.c_str());
                WritePrivateProfileStringW(L"Studio", L"cleanupDefaultsVersion", L"2", defaultsPath.c_str());
            }
            // Adopt the approved R470 allocation baseline once. Preserve the
            // user's hidden reconstruction settings and subsequent edits.
            if(GetPrivateProfileIntW(L"Studio",L"allocationDefaultsVersion",0,defaultsPath.c_str())<470) {
                WritePrivateProfileStringW(L"Studio",L"flatAllocation",L"50",defaultsPath.c_str());
                WritePrivateProfileStringW(L"Studio",L"flatSavings",L"0",defaultsPath.c_str());
                WritePrivateProfileStringW(L"Studio",L"flatThreshold",L"0",defaultsPath.c_str());
                WritePrivateProfileStringW(L"Studio",L"allocationDefaultsVersion",L"470",defaultsPath.c_str());
            }
            for (const auto& item : std::array<std::tuple<const wchar_t*, const wchar_t*, uint32_t, uint32_t>, 4>{
                     {{L"R441Intensity", L"r441Intensity", 50, 200},
                      {L"StrongerTangents", L"r441Tangents", 0, 100},
                      {L"R441Antialias", L"r441AA", 50, 100},
                      {L"R441Contrast", L"r441Contrast", 0, 100}}}) {
                auto path = settingsPath();
                const auto name = std::get<0>(item);
                auto value = std::min(std::get<3>(item), GetPrivateProfileIntW(L"Studio", std::get<1>(item),
                                                                               std::get<2>(item), path.c_str()));
                find<Slider>(name).Value(value);
                find<TextBlock>((std::wstring(name) + L"Value").c_str()).Text(std::to_wstring(value) + L" %");
            }
            find<ComboBox>(L"R441Style")
                .SelectedIndex(std::min(1u, GetPrivateProfileIntW(L"Studio", L"r441Style", 0, settingsPath().c_str())));
            find<CheckBox>(L"R441ProtectText")
                .IsChecked(GetPrivateProfileIntW(L"Studio", L"r441TextProtection", 0, settingsPath().c_str()) != 0);
            find<CheckBox>(L"R441Automatic")
                .IsChecked(GetPrivateProfileIntW(L"Studio",L"r441Automatic",1,settingsPath().c_str())!=0);
            for(auto [name,key]:{std::pair{L"ShapeTolerance",L"shapeTolerance"},std::pair{L"ShapeMinimumSize",L"shapeMinimumSize"}}){
                const auto value=std::clamp(GetPrivateProfileIntW(L"Studio",key,100,settingsPath().c_str()),25u,200u);
                find<Slider>(name).Value(value);
                find<TextBlock>((std::wstring(name)+L"Value").c_str()).Text(std::to_wstring(value)+L" %");
            }
            for (const auto& entry : encoderCatalog) {
                ComboBoxItem item;
                item.Content(box_value(entry.name));
                ToolTipService::SetToolTip(item, box_value(entry.summary));
                find<ComboBox>(L"CodecVersion").Items().Append(item);
            }
            find<ComboBox>(L"CodecVersion").SelectedIndex(0);
            window = Window();
            window.Title(L"Fajk Studio R600.4 · CPU / GPU");
            window.Content(root);
            check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
            window.AppWindow().Resize({1440, 940});
            std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app) << "window created\n";
            int count = 0;
            auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
            if (count > 1) {
                smokeFast=std::wstring(argv[1])==L"--smoke-fast-gpu";
                smokeGpu=smokeFast||std::wstring(argv[1])==L"--smoke-gpu";
                smokeHeatmap = std::wstring(argv[1]) == L"--smoke-heatmap";
                smokeDebug = std::wstring(argv[1]) == L"--smoke-debug";
                smoke = smokeGpu || smokeDebug || smokeHeatmap || std::wstring(argv[1]) == L"--smoke";
                if(smokeGpu){options.encode.backend=ComputeBackend::Gpu;decoderBackend=ComputeBackend::Gpu;}
                if (count > (smoke ? 2 : 1))
                    initial = argv[smoke ? 2 : 1];
            }
            LocalFree(argv);
            auto path = settingsPath();
            find<CheckBox>(L"AdaptiveQuantizationEnabled").IsChecked(GetPrivateProfileIntW(L"Studio",L"adaptiveQuantizationEnabled",0,path.c_str())!=0);
            if(smokeDebug||smokeHeatmap)find<CheckBox>(L"AdaptiveQuantizationEnabled").IsChecked(true);
            auto hw = hardwareInfo();
            std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app)
                << "hardware " << hw.logical << " " << hw.physical << " " << hw.safeMiB << "\n";
            if (GetPrivateProfileIntW(L"Studio", L"resourcesVersion", 0, path.c_str()) >= 3) {
                options.memoryMiB = GetPrivateProfileIntW(L"Studio", L"ram", 0, path.c_str());
                options.workers = std::min(hw.logical, GetPrivateProfileIntW(L"Studio", L"workers", 0, path.c_str()));
            }

            deblocking=std::min(100u,GetPrivateProfileIntW(L"Studio",L"deblocking",100,path.c_str()));
            sharpening=std::min(200u,GetPrivateProfileIntW(L"Studio",L"sharpening",0,path.c_str()));
            find<Slider>(L"Quality")
                .Value(std::clamp(GetPrivateProfileIntW(L"Studio", L"quality", 5, path.c_str()), 1u, 100u));
            find<Slider>(L"Volume").Value(
                std::min(100u, GetPrivateProfileIntW(L"Studio", L"volume", 70, path.c_str())));
            find<ComboBox>(L"CodecVersion").SelectedIndex([&] {
                auto revision =
                    GetPrivateProfileIntW(L"Studio", L"codecRevision", uint32_t(defaultEncoderRevision), path.c_str());
                if (!revision)
                    revision = GetPrivateProfileIntW(L"Studio", L"codecVersion", 1, path.c_str()) == 0 ? 13 : 12;
                return int(encoderIndex(EncoderRevision(revision),ComputeBackend(std::min(1u,GetPrivateProfileIntW(L"Studio",L"encodeBackend",uint32_t(options.encode.backend),path.c_str())))));
            }());
            if(smokeGpu){decoderBackend=ComputeBackend::Gpu;find<ComboBox>(L"CodecVersion").SelectedIndex(1);}
            decoderBackend=encodeOptions().backend;
            find<ComboBox>(L"EncodeSpeed").SelectedIndex(int(std::min(2u,GetPrivateProfileIntW(L"Studio",L"encodeSpeed",uint32_t(options.encode.speed),path.c_str()))));
            if(smokeFast)find<ComboBox>(L"EncodeSpeed").SelectedIndex(1);
            find<ComboBox>(L"TemporalMethod").SelectedIndex(std::min(2u,GetPrivateProfileIntW(L"Studio",L"temporalMethod",GetPrivateProfileIntW(L"Studio",L"coefficientTemporal",GetPrivateProfileIntW(L"Studio",L"similarTolerance",0,path.c_str())==0?1:0,path.c_str())?0:1,path.c_str())));
            find<ToggleSwitch>(L"Temporal").IsOn(GetPrivateProfileIntW(L"Studio",L"temporal",0,path.c_str())!=0);
            find<NumberBox>(L"KeyframeInterval").Value(std::min(uint32_t(maxFrames),GetPrivateProfileIntW(L"Studio",L"keyframeInterval",120,path.c_str())));
            find<NumberBox>(L"SimilarTolerance").Value(std::min(16u,GetPrivateProfileIntW(L"Studio",L"similarTolerance",0,path.c_str())));
            find<NumberBox>(L"MaxTolerance").Value(std::min(16u,GetPrivateProfileIntW(L"Studio",L"maxTolerance",6,path.c_str())));
            find<NumberBox>(L"SimilarHold").Value(std::clamp(GetPrivateProfileIntW(L"Studio",L"similarHold",10,path.c_str()),1u,120u));
            describeEncoder();
            find<ComboBox>(L"ChromaMode")
                .SelectedIndex(GetPrivateProfileIntW(L"Studio", L"chroma422", 0, path.c_str())   ? 2
                               : GetPrivateProfileIntW(L"Studio", L"chroma420", 1, path.c_str()) ? 0
                                                                                                 : 1);
            auto savedBudget = std::min(9u, GetPrivateProfileIntW(L"Studio", L"budget", 0, path.c_str()));
            if (GetPrivateProfileIntW(L"Studio", L"budgetVersion", 0, path.c_str()) < 2) {
                constexpr uint32_t legacy[]{0, 1, 3, 5, 6, 7};
                savedBudget = legacy[std::min(savedBudget, 5u)];
            }
            if (GetPrivateProfileIntW(L"Studio", L"budgetVersion", 0, path.c_str()) < 3) {
                constexpr uint32_t migrate[]{0, 2, 3, 4, 5, 6, 8, 9};
                savedBudget = migrate[std::min(savedBudget, 7u)];
            }
            find<ComboBox>(L"Budget").SelectedIndex(savedBudget);
            for (const auto& entry : find<ComboBox>(L"Budget").Items()) {
                ComboBoxItem item;
                item.Content(entry.as<ComboBoxItem>().Content());
                find<ComboBox>(L"DebugBudget").Items().Append(item);
            }
            find<ComboBox>(L"DebugBudget").SelectedIndex(savedBudget);
            find<ComboBox>(L"DebugBudget").SelectionChanged([this](auto&&, auto&&) {
                const auto selected = find<ComboBox>(L"DebugBudget").SelectedIndex();
                if (selected != find<ComboBox>(L"Budget").SelectedIndex())
                    find<ComboBox>(L"Budget").SelectedIndex(selected);
            });
            auto storedEvery = std::clamp(GetPrivateProfileIntW(L"Studio", L"every", 1, path.c_str()), 1u, 16u);
            int everyIndex = 0;
            while ((1u << everyIndex) < storedEvery)
                ++everyIndex;
            find<ComboBox>(L"Every").SelectedIndex(everyIndex);
            std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app) << "settings loaded\n";
            bind(L"Open", [this] { openSource(dialog(false)); });
            bind(L"OpenSequence", [this] { openSequence(dialog(false, true)); });
            bind(L"Attach", [this] { attach(); });
            bind(L"Preview", [this] { preview(); });
            bind(L"Convert", [this] { convertTo(dialog(true)); });
            bind(L"Cancel", [this] {
                cancelJob();
                status(L"Rušení operace…");
            });
            bind(L"Settings", [this] { settings(); });
            bind(L"Export", [this] { exportFrame(); });
            bind(L"Play", [this] { play(); });
            bind(L"Previous", [this] { step(-1); });
            bind(L"Next", [this] { step(1); });
            find<Slider>(L"Deblocking").Value(deblocking);
            auto filterLabel = [this](uint32_t value) {
                find<TextBlock>(L"DeblockingValue").Text(value ? std::to_wstring(value) + L" %" : L"Vypnuto · 0 %");
            };
            filterLabel(deblocking);
            find<Slider>(L"Deblocking").ValueChanged([this, filterLabel](auto&&, auto const& e) {
                deblocking = uint32_t(e.NewValue());
                filterLabel(deblocking);
                saveSettings();
                filterPending = legacyFilter || colorFilter || sharpFilter;
                filterChanged = std::chrono::steady_clock::now();
            });
            find<Slider>(L"Sharpening").Value(sharpening);
            find<TextBlock>(L"SharpeningValue")
                .Text(sharpening ? std::to_wstring(sharpening) + L" %" : L"Vypnuto · 0 %");
            find<Slider>(L"Sharpening").ValueChanged([this](auto&&, auto const& e) {
                const auto value = uint32_t(e.NewValue());
                sharpening=value;saveSettings();
                find<TextBlock>(L"SharpeningValue").Text(value ? std::to_wstring(value) + L" %" : L"Vypnuto · 0 %");
                filterPending = sharpFilter;
                filterChanged = std::chrono::steady_clock::now();
            });

            find<Slider>(L"ShapeIntensity")
                .Value(std::min(200u, GetPrivateProfileIntW(L"Studio", L"shapeIntensity", 200, path.c_str())));
            find<TextBlock>(L"ShapeIntensityValue")
                .Text(std::to_wstring(uint32_t(find<Slider>(L"ShapeIntensity").Value())) + L" %");
            find<Slider>(L"ShapeIntensity").ValueChanged([this](auto&&, auto const& e) {
                find<TextBlock>(L"ShapeIntensityValue").Text(std::to_wstring(uint32_t(e.NewValue())) + L" %");
                settingsChanged();
            });
            find<Slider>(L"ContourIntensity")
                .Value(std::min(200u, GetPrivateProfileIntW(L"Studio", L"contourIntensity", 0, path.c_str())));
            find<TextBlock>(L"ContourIntensityValue")
                .Text(std::to_wstring(uint32_t(find<Slider>(L"ContourIntensity").Value())) + L" %");
            find<Slider>(L"ContourIntensity").ValueChanged([this](auto&&, auto const& e) {
                find<TextBlock>(L"ContourIntensityValue").Text(std::to_wstring(uint32_t(e.NewValue())) + L" %");
                settingsChanged();
            });
            find<Slider>(L"FlatAllocation")
                .Value(std::min(100u, GetPrivateProfileIntW(L"Studio", L"flatAllocation", 50, path.c_str())));
            find<TextBlock>(L"FlatAllocationValue")
                .Text(std::to_wstring(uint32_t(find<Slider>(L"FlatAllocation").Value())) + L" %");
            find<Slider>(L"FlatAllocation").ValueChanged([this](auto&&, auto const& e) {
                find<TextBlock>(L"FlatAllocationValue").Text(std::to_wstring(uint32_t(e.NewValue())) + L" %");
                settingsChanged();
            });
            find<Slider>(L"FlatSavings").Value(std::min(100u, GetPrivateProfileIntW(L"Studio", L"flatSavings", 0, path.c_str())));
            find<TextBlock>(L"FlatSavingsValue").Text(std::to_wstring(uint32_t(find<Slider>(L"FlatSavings").Value())) + L" %");
            find<Slider>(L"FlatSavings").ValueChanged([this](auto&&, auto const& e) {
                find<TextBlock>(L"FlatSavingsValue").Text(std::to_wstring(uint32_t(e.NewValue())) + L" %");
                settingsChanged();
            });
            find<Slider>(L"FlatThreshold").Value(std::min(100u,GetPrivateProfileIntW(L"Studio",L"flatThreshold",0,path.c_str())));
            find<TextBlock>(L"FlatThresholdValue").Text(std::to_wstring(uint32_t(find<Slider>(L"FlatThreshold").Value()))+L" %");
            find<Slider>(L"FlatThreshold").ValueChanged([this](auto&&,auto const& e) {
                find<TextBlock>(L"FlatThresholdValue").Text(std::to_wstring(uint32_t(e.NewValue()))+L" %");
                settingsChanged();
            });
            for (auto name : debugViewNames) {
                find<ComboBox>(L"DebugLeft").Items().Append(box_value(name));
                find<ComboBox>(L"DebugRight").Items().Append(box_value(name));
            }
            find<ComboBox>(L"DebugLeft").SelectedIndex(0);
            find<ComboBox>(L"DebugRight").SelectedIndex(2);
            bind(L"DebugTab", [this] { debugTab(!debugMode); });
            bind(L"DebugInspect", [this] { requestDebug(); });
            bind(L"DebugClose", [this] { debugTab(false); });
            bind(L"DebugLeftHelp", [this] { debugViewHelp(0); });
            bind(L"DebugRightHelp", [this] { debugViewHelp(1); });
            for (int slot = 0; slot < 2; ++slot)
                find<ComboBox>(slot ? L"DebugRight" : L"DebugLeft")
                    .AddHandler(UIElement::PointerWheelChangedEvent(),
                                box_value(Input::PointerEventHandler(
                                    [this, slot](auto&&, Input::PointerRoutedEventArgs const& e) {
                                        auto box = find<ComboBox>(slot ? L"DebugRight" : L"DebugLeft");
                                        if (box.IsDropDownOpen())
                                            return; // Let the open list scroll normally.
                                        cycleDebugView(slot, e.GetCurrentPoint(box).Properties().MouseWheelDelta());
                                        e.Handled(true);
                                    })),
                                true);
            for (auto name : {L"DebugLeft", L"DebugRight"})
                find<ComboBox>(name).SelectionChanged([this](auto&&, auto&&) { requestDebug(); });
            bind(L"CaptureBudgetMaps", [this] { captureBudgetMaps(); });
            find<ComboBox>(L"HeatmapKind").SelectionChanged([this](auto&&, auto&&) {
                if (debugMode) {
                    invalidateDebug();
                    rebuildBudgetMap();
                    render();
                }
            });
            bind(L"DebugNormal", [this] { debugTab(false); });
            find<Slider>(L"DebugGain").ValueChanged([this](auto&&, auto const& e) {
                find<TextBlock>(L"DebugGainValue").Text(std::to_wstring(int(e.NewValue())) + L"×");
                requestDebug();
            });
            find<ToggleSwitch>(L"HeatmapToggle").Toggled([this](auto&&, auto&&) {
                if (find<ToggleSwitch>(L"HeatmapToggle").IsOn()) {
                    if (!budgetFrame && original && !busy)
                        preview();
                    else if (compressed)
                        setMode(1);
                }
                render();
            });
            find<Slider>(L"HeatmapOpacity").ValueChanged([this](auto&&, auto&&) { render(); });
            if (smokeHeatmap)
                debugTab(true);
            if (smokeHeatmap)
                find<ToggleSwitch>(L"HeatmapToggle").IsOn(true);
            bind(L"PanelToggle", [this] {
                sidebarVisible = !sidebarVisible;
                layout();
            });
            root.SizeChanged([this](auto&&, auto const& e) {
                if (!fullscreen) {
                    bool narrow = e.NewSize().Width < 1050;
                    if (narrow && sidebarVisible && !debugMode) {
                        sidebarVisible = false;
                        layout();
                    } else if (e.PreviousSize().Width < 1050 && !narrow) {
                        sidebarVisible = true;
                        layout();
                    }
                }
            });
            bind(L"Fullscreen", [this] { fullScreen(); });
            bind(L"Fit", [this] {
                fitMode = true;
                fit();
            });
            bind(L"Zoom", [this] { zoom(1.); });
            bind(L"ZoomIn", [this] { zoom(viewScale * 1.25); });
            bind(L"ZoomOut", [this] { zoom(viewScale / 1.25); });
            bind(L"Mute", [this] {
                if (audio) {
                    audio.IsMuted(!audio.IsMuted());
                    find<FontIcon>(L"VolumeIcon").Glyph(audio.IsMuted() ? L"\xE74F" : L"\xE767");
                }
            });
            find<Slider>(L"Volume").ValueChanged([this](auto&&, auto const& e) {
                if (audio) {
                    audio.Volume(e.NewValue() / 100.);
                    audio.IsMuted(false);
                }
                find<FontIcon>(L"VolumeIcon").Glyph(e.NewValue() == 0 ? L"\xE74F" : L"\xE767");
                saveSettings();
            });
            find<ComboBox>(L"ViewMode").SelectionChanged([this](auto&&, auto&&) {
                if (!settingMode) {
                    if (!sequencePath.empty()) {
                        stopPlay();
                        invalidateDecode();
                        requestFrame(current);
                    } else
                        render();
                }
            });
            find<Slider>(L"Split").ValueChanged([this](auto&&, auto&&) { split(); });
            find<ScrollViewer>(L"Viewport").SizeChanged([this](auto&&, auto&&) { fit(); });
            auto surface = find<Grid>(L"Surface");
            // Listen on the viewport so the wheel also works over letterboxed margins.
            find<ScrollViewer>(L"Viewport")
                .AddHandler(
                    UIElement::PointerWheelChangedEvent(),
                    box_value(Input::PointerEventHandler([this](auto&&, Input::PointerRoutedEventArgs const& e) {
                        if (!shown)
                            return;
                        const auto point = e.GetCurrentPoint(find<ScrollViewer>(L"Viewport"));
                        zoomAt(viewScale * std::pow(1.2, point.Properties().MouseWheelDelta() / 120.),
                               point.Position());
                        e.Handled(true);
                    })),
                    true);
            surface.PointerPressed([this](auto const& sender, Input::PointerRoutedEventArgs const& e) {
                if (!shown)
                    return;
                auto p = e.GetCurrentPoint(find<Grid>(L"Surface"));
                if (!p.Properties().IsLeftButtonPressed())
                    return;
                dragging = true;
                dragSplit = !(debugMode && debugFrame) && find<ComboBox>(L"ViewMode").SelectedIndex() == 2 &&
                            std::abs(p.Position().X -
                                     find<Grid>(L"Surface").Width() * find<Slider>(L"Split").Value() / 100) < 22;
                auto location = e.GetCurrentPoint(find<ScrollViewer>(L"Viewport")).Position();
                dragX = location.X;
                dragY = location.Y;
                offsetX = find<ScrollViewer>(L"Viewport").HorizontalOffset();
                offsetY = find<ScrollViewer>(L"Viewport").VerticalOffset();
                sender.template as<UIElement>().CapturePointer(e.Pointer());
                e.Handled(true);
            });
            surface.PointerMoved([this](auto&&, Input::PointerRoutedEventArgs const& e) {
                if (debugMode && budgetMap && shown) {
                    auto point = e.GetCurrentPoint(find<Grid>(L"Surface")).Position();
                    const auto surface = find<Grid>(L"Surface");
                    double nx = point.X / std::max(1., surface.Width()), ny = point.Y / std::max(1., surface.Height());
                    if (debugFrame) {
                        const double pane = (debugFrame->width - 16) / 2.;
                        double pixel = nx * debugFrame->width;
                        if (pixel >= pane + 16)
                            pixel -= pane + 16;
                        nx = pixel / pane;
                    }
                    if (nx >= 0 && nx < 1 && ny >= 0 && ny < 1) {
                        const auto info = to_hstring(budgetCellInfo(*budgetMap, uint32_t(nx * budgetMap->width),
                                                                    uint32_t(ny * budgetMap->height)));
                        find<TextBlock>(L"DebugProbe").Text(info);
                        find<TextBlock>(L"DebugPixelProbe").Text(info);
                    }
                }

                if (!dragging)
                    return;
                if (dragSplit) {
                    find<Slider>(L"Split").Value(std::clamp(
                        100. * e.GetCurrentPoint(find<Grid>(L"Surface")).Position().X / find<Grid>(L"Surface").Width(),
                        0., 100.));
                } else {
                    auto view = find<ScrollViewer>(L"Viewport");
                    auto p = e.GetCurrentPoint(view).Position();
                    view.ChangeView(offsetX + dragX - p.X, offsetY + dragY - p.Y, nullptr, true);
                }
                e.Handled(true);
            });
            surface.PointerReleased([this](auto const& sender, Input::PointerRoutedEventArgs const& e) {
                dragging = false;
                sender.template as<UIElement>().ReleasePointerCapture(e.Pointer());
            });
            surface.PointerCaptureLost([this](auto&&, auto&&) { dragging = false; });
            surface.DoubleTapped([this](auto&&, auto&&) {
                if (shown)
                    fullScreen();
            });
            root.KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& e) {
                using K = winrt::Windows::System::VirtualKey;
                auto key = e.Key();
                if (key == K::Escape && fullscreen) {
                    fullScreen();
                    e.Handled(true);
                    return;
                }
                auto focus = Input::FocusManager::GetFocusedElement(root.XamlRoot());
                if (focus && (focus.try_as<Slider>() || focus.try_as<ComboBox>() || focus.try_as<TextBox>() ||
                              focus.try_as<NumberBox>()))
                    return;
                if (key == K::Space) {
                    play();
                    e.Handled(true);
                } else if (key == K::Left) {
                    step(-1);
                    e.Handled(true);
                } else if (key == K::Right) {
                    step(1);
                    e.Handled(true);
                } else if (key == K::F && shown) {
                    fullScreen();
                    e.Handled(true);
                } else if (key == K::M && audio) {
                    audio.IsMuted(!audio.IsMuted());
                    find<FontIcon>(L"VolumeIcon").Glyph(audio.IsMuted() ? L"\xE74F" : L"\xE767");
                    e.Handled(true);
                }
            });
            for (auto name : {L"R441Intensity", L"StrongerTangents", L"R441Antialias", L"R441Contrast"})
                find<Slider>(name).ValueChanged([this, name](auto&&, auto const& e) {
                    find<TextBlock>((std::wstring(name) + L"Value").c_str())
                        .Text(std::to_wstring(uint32_t(e.NewValue())) + L" %");
                    settingsChanged();
                });
            find<ComboBox>(L"R441Style").SelectionChanged([this](auto&&, auto&&) { settingsChanged(); });
            find<CheckBox>(L"R441ProtectText").Checked([this](auto&&, auto&&) { settingsChanged(); });
            find<CheckBox>(L"R441ProtectText").Unchecked([this](auto&&, auto&&) { settingsChanged(); });
            find<CheckBox>(L"R441Automatic").Checked([this](auto&&,auto&&){settingsChanged();});
            find<CheckBox>(L"R441Automatic").Unchecked([this](auto&&,auto&&){settingsChanged();});
            for(auto name:{L"ShapeTolerance",L"ShapeMinimumSize"})
                find<Slider>(name).ValueChanged([this,name](auto&&,auto const& e){
                    find<TextBlock>((std::wstring(name)+L"Value").c_str()).Text(std::to_wstring(uint32_t(e.NewValue()))+L" %");
                    settingsChanged();
                });
            bind(L"ResetShapeTuning",[this]{find<Slider>(L"ShapeTolerance").Value(100);find<Slider>(L"ShapeMinimumSize").Value(100);});
            find<CheckBox>(L"AdaptiveQuantizationEnabled").Checked([this](auto&&, auto&&) { settingsChanged(); updateEnabled(); });
            find<CheckBox>(L"AdaptiveQuantizationEnabled").Unchecked([this](auto&&, auto&&) { settingsChanged(); updateEnabled(); });
            bind(L"EncoderHelp", [this] { encoderHelp(); });
            find<ComboBox>(L"CodecVersion").SelectionChanged([this](auto&&,auto&&){
                const auto selected=encodeOptions().backend;const bool changed=decoderBackend!=selected;
                decoderBackend=selected;settingsChanged();
                if(changed&&!smoke){stopPlay();invalidateDecode();
                    if(!sequencePath.empty())requestFrame(current);
                    else if(previewPacket&&original&&!busy)refreshFilter();}
            });
            find<ComboBox>(L"EncodeSpeed").SelectionChanged([this](auto&&, auto&&) { settingsChanged(); });
            find<Slider>(L"Quality").ValueChanged([this](auto&&, auto&&) { settingsChanged(); });
            find<ComboBox>(L"ChromaMode").SelectionChanged([this](auto&&, auto&&) { settingsChanged(); });
            find<ComboBox>(L"Budget").SelectionChanged([this](auto&&, auto&&) {
                if (find<ComboBox>(L"DebugBudget").SelectedIndex() != find<ComboBox>(L"Budget").SelectedIndex())
                    find<ComboBox>(L"DebugBudget").SelectedIndex(find<ComboBox>(L"Budget").SelectedIndex());
                settingsChanged();
            });
            find<ComboBox>(L"Every").SelectionChanged([this](auto&&, auto&&) { settingsChanged(); });
            find<ComboBox>(L"TemporalMethod").SelectionChanged([this](auto&&,auto&&){saveSettings();updateEnabled();});
            find<ToggleSwitch>(L"Temporal").Toggled([this](auto&&,auto&&){saveSettings();updateEnabled();});
            find<NumberBox>(L"KeyframeInterval").ValueChanged([this](auto&&,auto&&){saveSettings();});
            for(const auto name:{L"SimilarTolerance",L"SimilarHold",L"MaxTolerance"})find<NumberBox>(name).ValueChanged([this](auto&&,auto&&){saveSettings();});
            find<Slider>(L"Timeline").ValueChanged([this](auto&&, auto const& args) {
                if (!settingTimeline) {
                    stopPlay();
                    invalidateDecode();
                    requestFrame(uint64_t(args.NewValue()));
                }
            });
            find<ComboBox>(L"Step").SelectionChanged([this](auto&&, auto&&) {
                if (playing) {
                    stopPlay();
                    play();
                }
            });
            find<ComboBox>(L"Speed").SelectionChanged([this](auto&&, auto&&) {
                if (playing) {
                    stopPlay();
                    play();
                }
            });
            std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app) << "events bound\n";
            root.AllowDrop(true);
            root.DragOver([this](auto&&, DragEventArgs const& e) {
                if (!busy && e.DataView().Contains(
                                 winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
                    e.AcceptedOperation(winrt::Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
            });
            root.Drop([this](auto&&, auto const& e) { drop(e); });
            window.Closed([this](auto&&, auto&&) { shutdown(); });
            startDecoder();
            timer = DispatcherTimer();
            timer.Interval(std::chrono::milliseconds(15));
            timer.Tick([this](auto&&, auto&&) {
                try {
                    if (!playing || smoke)
                        tick();
                } catch (hresult_error const& e) {
                    stopPlay();
                    error(e.message().c_str());
                } catch (std::exception const& e) {
                    stopPlay();
                    error(to_hstring(e.what()).c_str());
                }
            });
            timer.Start();
            renderingToken = CompositionTarget::Rendering([this](auto&&, auto&&) {
                if (playing && !smoke)
                    try {
                        tick();
                    } catch (hresult_error const& e) {
                        stopPlay();
                        error(e.message().c_str());
                    } catch (std::exception const& e) {
                        stopPlay();
                        error(to_hstring(e.what()).c_str());
                    }
            });
            window.Activate();
            if (smoke)
                window.AppWindow().Hide();
            settingsChanged();
            updateEnabled();
            std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app) << "activated\n";
            if(smoke)smokeSkirtControls();
            if (!initial.empty()) {
                if (fajkFile(initial))
                    openSequence(initial);
                else
                    openSource(initial);
            }
        } catch (hresult_error const& e) {
            std::ofstream(executableDirectory() / L"startup-error.txt")
                << to_string(e.message()) << " HRESULT " << uint32_t(e.code());
            Exit();
        } catch (std::exception const& e) {
            std::ofstream(executableDirectory() / L"startup-error.txt") << e.what();
            Exit();
        }
    }
};
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        std::ofstream(executableDirectory() / L"startup-trace.txt") << "entry\n";
        init_apartment(apartment_type::single_threaded);
        std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app) << "apartment\n";
        Application::Start([](auto&&) {
            std::ofstream(executableDirectory() / L"startup-trace.txt", std::ios::app) << "make\n";
            make<Studio>();
        });
        return 0;
    } catch (hresult_error const& e) {
        std::ofstream(executableDirectory() / L"startup-error.txt") << to_string(e.message());
        MessageBoxW(nullptr, e.message().c_str(), L"Fajk Studio", MB_ICONERROR);
        return 1;
    } catch (std::exception const& e) {
        std::ofstream(executableDirectory() / L"startup-error.txt") << e.what();
        MessageBoxW(nullptr, to_hstring(e.what()).c_str(), L"Fajk Studio", MB_ICONERROR);
        return 1;
    }
}
