#include "debug_inspection.hpp"
#include "fajk/codec.hpp"
#include "fajk/container.hpp"
#include "fajk/pipeline.hpp"
#include "fajk/media.hpp"
#include "fajk/hardware.hpp"
#include "decode_internal.hpp"
#include "timing.hpp"
#include "fajk/compute.hpp"
#include "output_safety.hpp"
#include "compression_report.hpp"
#include "artifact_cleanup.hpp"
#include "artifact_reconstruction.hpp"
#include "reconstruction441.hpp"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <windows.h>
#include <stop_token>
using namespace fajk;
namespace {
std::stop_source cancellation;
BOOL WINAPI cancel(DWORD code) {
    if (code == CTRL_C_EVENT || code == CTRL_BREAK_EVENT) {
        cancellation.request_stop();
        return TRUE;
    }
    return FALSE;
}
uint64_t number(const wchar_t* text) {
    std::wstring value(text);
    require(!value.empty() && value[0] != L'-', "Expected unsigned integer.");
    size_t end = 0;
    auto n = std::stoull(value, &end);
    require(end == value.size(), "Invalid integer.");
    return n;
}
uint32_t integer(const wchar_t* text) {
    auto n = number(text);
    require(n <= UINT32_MAX, "Integer exceeds 32 bits.");
    return uint32_t(n);
}
double elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
struct CpuWork {uint64_t time=0,cycles=0;};
CpuWork cpuWork(){FILETIME creation,exit,kernel,user;CpuWork result;
    if(GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)){
        ULARGE_INTEGER k{},u{};k.LowPart=kernel.dwLowDateTime;k.HighPart=kernel.dwHighDateTime;
        u.LowPart=user.dwLowDateTime;u.HighPart=user.dwHighDateTime;result.time=k.QuadPart+u.QuadPart;}
    QueryProcessCycleTime(GetCurrentProcess(),&result.cycles);return result;
}
void help() {
    std::cout << "Fajk Studio R600.4 / R600.4 GPU\n"
                 "Use --gpu or --cpu with encode, decode, measure and measure-decode.\n"
                 "Encode speed: --more-quality (default), --fast or --fast-max (Fast-based Max compression); independent of CPU/GPU.\n"
                 "AQ: --enable-aq / --no-aq (default off); --aq N controls strength only. Text protection defaults off.\n"
                 "Video: --temporal [--keyframe-interval N], 0 disables the frame-age limit; changes refresh within 5 s, exact repeats can hold indefinitely.\n"
                 "Optional similar regions: --similar-tolerance 0..16 --similar-hold 1..120 (default 0/off, 10 frames).\n"
                 "Lossless interframe recoding: --temporal-coefficients (identical pixels, <=16 dependencies, static repeats are direct; similarity tolerance must be 0).\n"
                 "Max compression: --temporal-max [--similar-tolerance 0..16] (default 6), --similar-hold 1..120; source-checked GPU block/affine motion, bounded residuals and exact coefficient fallback when motion quality fails.\n"
                 "encode INPUT [OUTPUT.fajk|OUTPUT.fajkv] [--quality 85] [--target-bytes BYTES | --no-target] "
                 "[--420|--422|--444] [--lossless] [--no-aq] [--workers N] [--overwrite]\n"
                 "  --r460-intensity 0-200 --stronger-tangents 0-100 --antialias 0-100 --contour-contrast 0-100 "
                 "--r460-style 0|1 --protect-text 0|1\n"
                 "  --contour-intensity 0-200 --shape-intensity 0-200 --aq 0-1000 --allocation-strength 0-500 "
                 "--flat-allocation 0-100 --flat-savings 0-500 --flat-threshold 0-100 --color-edge-priority 0-500\n"
                 "decode INPUT FRAME OUTPUT.png [--workers N] [--deblock 0-100] [--sharpen 0-200]\n"
                 "info INPUT | hardware | encoders\n"
                 "inspect-frame INPUT PREFIX [LEFT=0] [RIGHT=2] [GAIN=1] [FRAME=0] (debug PNG/report)\n"
                 "inspect-compression INPUT  (uniform mode-23 band report)\n"
                 "inspect-contours INPUT PREFIX [GRID=auto] [baseline]  (native/effect PNGs and guide/rejection CSV)\n"
                 "inspect-allocation INPUT PREFIX TARGET_BYTES CONTRAST [compare0 [STRENGTH [AQ_ENABLED [THRESHOLD]]]]\n"
                 "measure-decode INPUT PREFIX REPEATS [WORKERS] [buffer]\n"
                 "measure INPUT PREFIX QUALITY REPEATS [420|422|444] [WORKERS] [DEBLOCKING] [wavelet] [TARGET_BYTES] "
                 "[SHARPENING] [r590|r590-buffer] [FLAT_ALLOCATION_0_100] [AQ_ENABLED_0_1] [FLAT_SAVINGS_0_100] [FLAT_THRESHOLD_0_100] [RECONSTRUCTION_INI]\n"
                 "R600.4 uses CPU/AVX2 or GPU reconstruction and optional temporal corrections. "
                 "Default base sharpening is 0.\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        MediaRuntime runtime;
        SetConsoleCtrlHandler(cancel, TRUE);
        if (argc < 2) {
            help();
            return 0;
        }
        ComputeBackend backend=ComputeBackend::Cpu;
        EncodeSpeed speed=EncodeSpeed::MoreQuality;
        int remaining=2;
        for(int i=2;i<argc;++i){const std::wstring key=argv[i];
            if(key==L"--gpu"||key==L"--r470-gpu"||key==L"--r480-gpu"||key==L"--r490-gpu"||key==L"--r500-gpu"||key==L"--r510-gpu"||key==L"--r520-gpu"||key==L"--r550-gpu"||key==L"--r560-gpu"||key==L"--r570-gpu"||key==L"--r580-gpu"||key==L"--r590-gpu"||key==L"--r600-gpu")backend=ComputeBackend::Gpu;
            else if(key==L"--cpu")backend=ComputeBackend::Cpu;
            else if(key==L"--fast")speed=EncodeSpeed::Fast;
            else if(key==L"--fast-max")speed=EncodeSpeed::MaxCompression;
            else if(key==L"--more-quality")speed=EncodeSpeed::MoreQuality;
            else argv[remaining++]=argv[i];}
        argc=remaining;
        const std::wstring command = argv[1];
        if (command == L"encode") {
            require(argc >= 3, "Missing input.");
            ConvertOptions options;
            bool toleranceSpecified=false;
            options.encode.backend=backend;
            options.encode.speed=speed;
            options.targetIncludesContainer = true;
            options.encode.automaticTarget = true;
            const bool explicitOutput = argc > 3 && std::wstring(argv[3]).rfind(L"--", 0) != 0;
            for (int i = explicitOutput ? 4 : 3; i < argc; ++i) {
                const std::wstring key = argv[i];
                if (key == L"--r441" || key == L"--r450" || key == L"--r460" || key == L"--r461" || key == L"--r462" || key == L"--r464" || key == L"--r465" || key == L"--r470" || key == L"--r480" || key == L"--r490" || key == L"--r500" || key == L"--r510" || key == L"--r520" || key == L"--r550" || key == L"--r560" || key == L"--r570" || key == L"--r580" || key == L"--r590" || key == L"--r600")
                    continue;
                if (key == L"--overwrite") {
                    options.replace = true;
                    continue;
                }
                if(key==L"--temporal"){options.temporal=true;options.coefficientTemporal=false;options.maxCompressionTemporal=false;continue;}
                if(key==L"--temporal-coefficients"){options.temporal=true;options.coefficientTemporal=true;options.maxCompressionTemporal=false;continue;}
                if(key==L"--temporal-max"){options.temporal=true;options.coefficientTemporal=false;options.maxCompressionTemporal=true;if(!toleranceSpecified)options.similarTolerance=6;continue;}
                if(key==L"--intra-only"){options.temporal=false;options.coefficientTemporal=false;options.maxCompressionTemporal=false;continue;}
                if (key == L"--enable-aq") { options.encode.adaptiveQuantizationEnabled=true;continue; }
                if (key == L"--no-aq") {
                    options.encode.adaptiveQuantizationEnabled = false;
                    continue;
                }
                if (key == L"--lossless") {
                    options.encode.lossless = true;
                    options.encode.automaticTarget = false;
                    continue;
                }
                if (key == L"--no-target") {
                    options.encode.targetBytes = 0;
                    options.encode.automaticTarget = false;
                    continue;
                }
                if (key == L"--420" || key == L"--422" || key == L"--444") {
                    options.autoChroma = false;
                    options.encode.chroma420 = key == L"--420";
                    options.encode.chroma422 = key == L"--422";
                    continue;
                }
                require(i + 1 < argc, "Missing option value.");
                const auto value = integer(argv[++i]);
                auto& e = options.encode;
                auto& s = e.reconstruction441;
                if (key == L"--quality")
                    e.quality = float(value);
                else if (key == L"--target-bytes")
                    e.targetBytes = value;
                else if (key == L"--workers")
                    options.workers = value;
                else if (key == L"--width")
                    options.maxWidth = value;
                else if (key == L"--every")
                    options.every = value;
                else if(key==L"--keyframe-interval")
                    options.keyframeInterval=value;
                else if(key==L"--similar-tolerance"){
                    options.similarTolerance=value;toleranceSpecified=true;}
                else if(key==L"--similar-hold")
                    options.similarHoldFrames=value;
                else if (key == L"--max-frames")
                    options.maxFrameCount = value;
                else if (key == L"--ram")
                    options.memoryMiB = value;
                else if ((key == L"--r441-intensity" || key == L"--r450-intensity" || key == L"--r460-intensity"))
                    s.intensity = value;
                else if (key == L"--stronger-tangents")
                    s.strongerTangents = value;
                else if (key == L"--antialias")
                    s.antialias = value;
                else if (key == L"--contour-contrast")
                    s.contrast = value;
                else if ((key == L"--r441-style" || key == L"--r450-style" || key == L"--r460-style"))
                    s.style = value;
                else if (key == L"--protect-text") {
                    require(value <= 1, "Text protection must be 0 or 1.");
                    s.protectText = value != 0;
                } else if (key == L"--contour-intensity")
                    e.contourIntensity = value;
                else if (key == L"--shape-intensity")
                    e.shapeIntensity = value;
                else if (key == L"--aq")
                    e.adaptiveQuantization = value;
                else if (key == L"--allocation-strength")
                    e.adaptiveAllocationStrength = value;
                else if (key == L"--flat-allocation")
                    e.flatAllocation = value;
                else if (key == L"--flat-threshold")
                    e.flatThreshold = value;
                else if (key == L"--flat-savings")
                    e.flatSavings = value;
                else if (key == L"--color-edge-priority")
                    e.colorEdgePriority = value;
                else
                    throw std::runtime_error("Unknown or retired encode option.");
            }
            Path output = explicitOutput
                              ? Path(argv[3])
                              : Path(argv[2]).parent_path() / (Path(argv[2]).stem().wstring() + L"_R600.4.fajk");
            const auto progress = convert(argv[2], output, options, cancellation.get_token());
            std::cout << "Saved " << progress.frames << " frames, " << progress.bytes << " bytes\n";
        } else if (command == L"decode") {
            require(argc >= 5 && (argc - 5) % 2 == 0, "decode INPUT FRAME OUTPUT.png");
            requireDifferentFiles(argv[2], argv[4]);
            SequenceReader reader(argv[2]);
            auto codec = makeCodec(reader.info().codec);
            codec->setComputeBackend(backend);
            for (int i = 5; i < argc; i += 2) {
                const std::wstring key = argv[i];
                const auto n = integer(argv[i + 1]);
                if (key == L"--deblock")
                    codec->setDeblocking(n);
                else if (key == L"--sharpen")
                    codec->setSharpening(n);
                else if (key == L"--workers")
                    codec->setThreads(n);
                else
                    throw std::runtime_error("Unknown decode option.");
            }
            writePng(argv[4], codec->decode(reader.read(number(argv[3])), reader.info().width, reader.info().height));
        } else if (command == L"inspect-frame") {
            require(argc >= 4 && argc <= 8, "inspect-frame INPUT PREFIX [LEFT=0] [RIGHT=2] [GAIN=1] [FRAME=0]");
            const Path prefix = argv[3];
            requireDifferentFiles(argv[2], prefix.wstring() + L"-debug.png");
            requireDifferentFiles(argv[2], prefix.wstring() + L"-debug.txt");
            SequenceReader reader(argv[2]);
            const auto& info = reader.info();
            const auto packet = reader.read(argc >= 8 ? number(argv[7]) : 0);
            const std::array<int, 2> views{argc >= 5 ? int(integer(argv[4])) : 0,
                                           argc >= 6 ? int(integer(argv[5])) : 2};
            auto debug =
                inspectPacket(packet, info.width, info.height, views, argc >= 7 ? float(integer(argv[6])) : 1.f);
            writePng(prefix.wstring() + L"-debug.png", debug.comparison);
            writeAtomicText(prefix.wstring() + L"-debug.txt", debug.report, true);
        } else if (command == L"inspect-contours") {
            require(argc >= 4 && argc <= 6, "inspect-contours INPUT PREFIX [GRID=auto] [baseline]");
            const bool recovery = argc == 4 || std::wstring(argv[argc - 1]) != L"baseline";
            require(argc < 6 || !recovery, "Invalid contour diagnostic mode.");
            const bool explicitGrid = argc >= 5 && (recovery || argc == 6) && std::wstring(argv[4]) != L"auto";
            const Path prefix = argv[3];
            requireDifferentFiles(argv[2], prefix.wstring() + L"-native.png");
            requireDifferentFiles(argv[2], prefix.wstring() + L"-guides.csv");
            requireDifferentFiles(argv[2], prefix.wstring() + L"-effect.png");
            SequenceReader reader(argv[2]);
            const auto packet = reader.read(0);
            const auto& info = reader.info();
            const auto view = read441(packet.data, info.width, info.height);
            DeferredUpscale deferred;
            deferred.skipBaseCleanup = true;
            const auto frame =
                decodeFrameDeferred({view.child, packet.pts, packet.duration}, info.width, info.height, 0, deferred);
            // read441 and the native decode have already validated this base.
            ByteReader base{view.child};
            base.bytes(6);
            const auto surfaces = base.u16();
            base.bytes(8);
            const auto budget = base.u32();
            const auto contours = base.u16();
            const float rate = artifactReconstruction::rateStrength(budget, info.width, info.height);
            const double equivalent = double(budget) * double(2560 * 1440) / (double(info.width) * info.height);
            const float amount = artifactReconstruction::contourStrength(rate, equivalent, contours);
            std::ofstream report(prefix.wstring() + L"-guides.csv");
            require(bool(report), "Cannot write guide report.");
            const auto grid =
                explicitGrid ? integer(argv[4]) : contourGuideSpacing(rate * surfaces / 100.f, amount);
            reportContourGuides(frame, grid, report, recovery, amount);
            writePng(prefix.wstring() + L"-native.png", frame);
            writePng(prefix.wstring() + L"-effect.png", contourEffectMap(frame, grid, amount, recovery));
        } else if (command == L"inspect-compression") {
            require(argc == 3, "inspect-compression INPUT");
            reportCoefficientCompression(argv[2], std::cout);
        } else if (command == L"info") {
            require(argc == 3, "info INPUT");
            SequenceReader reader(argv[2]);
            const auto& i = reader.info();
            std::cout << i.width << 'x' << i.height << " | " << i.count << " frames | " << i.duration / 1e7
                      << " seconds\n";
            if(i.temporal){uint64_t keys=0;for(const auto& entry:reader.index())keys+=entry.reference==0;
                std::cout<<"Temporal: "<<keys<<" keyframes, "<<i.count-keys<<" delta/repeat frames\n";}
        } else if (command == L"encoders") {
            std::cout << "56 r600 R600.4 CPU\n56 r600-gpu R600.4 GPU (--gpu; use the supplied decoder for temporal video)\n";
        } else if (command == L"hardware") {
            const auto h = hardwareInfo();
            std::cout << h.physical << " physical / " << h.logical << " logical cores, " << h.safeMiB
                      << " MiB safe RAM\n";
            const auto gpu=gpuInfo();std::cout<<"GPU: "<<gpu.device<<" | "<<(gpu.available?"available":gpu.error)<<'\n';
        } else if (command == L"measure-decode") {
            require(argc >= 5 && argc <= 7, "measure-decode INPUT PREFIX REPEATS [WORKERS] [buffer]");
            SequenceReader reader(argv[2]);
            const auto packet = reader.read(0);
            const auto& info = reader.info();
            const auto count = integer(argv[4]), workers = argc >= 6 ? integer(argv[5]) : 0;
            require(count > 0 && count <= 10000, "Invalid repeat count.");
            Path prefix = argv[3];
            std::ofstream csv(prefix.wstring() + L".csv");
            require(bool(csv), "Cannot write timings.");
            csv << "repeat,decode_ms,cpu_ms,cpu_cycles,gpu_transforms,gpu_uploaded_bytes,gpu_downloaded_bytes,gpu_fallbacks,gpu_contour_filters\n";
            Frame frame;
            const bool direct = argc == 7;
            require(!direct || std::wstring(argv[6]) == L"buffer", "Invalid decode measurement mode.");
            if (direct)
                frame = {info.width, info.height, packet.pts, packet.duration,
                         Bytes(pixelBytes(info.width, info.height))};
            std::ofstream stages(prefix.wstring() + L"-stages.csv");
            require(bool(stages), "Cannot write decode profile.");
            stages << "repeat,stage,ms\n";
            for (int repeat = -2; repeat < int(count); ++repeat) {
                std::vector<TimingEvent> events;
                TimingCapture capture(&events);
                const auto workStart=cpuWork();const auto gpuStart=gpuActivity();
                const auto start = std::chrono::steady_clock::now();
                if (direct)
                    decodeFrameToBuffer(packet, info.width, info.height, workers, 100, frame.pixels, 0, true, backend);
                else
                    frame = decodeFrameWithSettings(packet, info.width, info.height, workers, 100, 0, true, backend);
                const auto ms = elapsed(start);
                if (repeat >= 0) {
                    const auto work=cpuWork();const auto gpu=gpuActivity();
                    csv << repeat << ',' << ms << ',' << (work.time-workStart.time)/10000. << ',' << work.cycles-workStart.cycles << ',' << gpu.transforms-gpuStart.transforms << ',' << gpu.uploadedBytes-gpuStart.uploadedBytes << ',' << gpu.downloadedBytes-gpuStart.downloadedBytes << ',' << gpu.fallbacks-gpuStart.fallbacks << ',' << gpu.contourFilters-gpuStart.contourFilters << '\n';
                    for (const auto& event : events)
                        stages << repeat << ',' << event.stage << ',' << event.ms << '\n';
                }
            }
            writePng(prefix.wstring() + L".png", frame);
        } else if (command == L"inspect-allocation") {
            require(argc >= 6 && argc <= 10 && (argc < 7 || std::wstring(argv[6]) == L"compare0"),
                    "inspect-allocation INPUT PREFIX TARGET_BYTES CONTRAST [compare0 [STRENGTH [AQ_ENABLED [THRESHOLD]]]]");
            const auto source = readImage(argv[2]);
            const Path prefix = argv[3];
            EncodeOptions o;
            o.backend=backend;
            o.speed=speed;
            o.quality = 100;
            o.targetBytes = integer(argv[4]);
            o.flatAllocation = integer(argv[5]);
            o.flatSavings = argc >= 8 ? integer(argv[7]) : 0;
            require(o.flatSavings <= 100, "Flat savings must be 0-100.");
            const auto aq = argc >= 9 ? integer(argv[8]) : 1;
            require(aq <= 1, "AQ enabled must be 0 or 1.");
            o.adaptiveQuantizationEnabled = aq != 0;
            o.flatThreshold = argc >= 10 ? integer(argv[9]) : 0;
            require(o.flatThreshold <= 100,"Flat threshold must be 0-100.");
            o.budgetHeatmap = std::make_shared<BudgetHeatmap>();
            auto codec = makeCodec(CodecId::Fajk);
            codec->setComputeBackend(backend);
            const auto packet = codec->encode(source, o);
            if (argc >= 7 && o.adaptiveQuantizationEnabled && !o.budgetHeatmap->bits.empty()) {
                auto reference = o;
                if (o.flatSavings || o.flatThreshold) {reference.flatSavings = 0;reference.flatThreshold=0;}
                else reference.flatAllocation = 0;
                reference.budgetHeatmap = std::make_shared<BudgetHeatmap>();
                const auto baseline = codec->encode(source, reference);
                if (!reference.budgetHeatmap->bits.empty()) {
                    const auto s = compareFlatAllocation(*o.budgetHeatmap, *reference.budgetHeatmap);
                    std::ofstream report(prefix.wstring() + L"-savings.txt"), json(prefix.wstring() + L"-savings.json");
                    require(bool(report) && bool(json), "Cannot write flat savings comparison.");
                    report << flatAllocationSavingsText(s, o.flatSavings != 0 || o.flatThreshold != 0) << "\nWhole container: " << baseline.data.size() + 168
                           << " -> " << packet.data.size() + 168 << " bytes\n";
                    json << std::setprecision(12) << "{\"reference_flat_bits\":" << s.referenceBits
                         << ",\"current_flat_bits\":" << s.currentBits << ",\"flat_area_share\":" << s.areaShare
                         << ",\"reference_other_bits\":" << s.referenceOtherBits << ",\"current_other_bits\":"
                         << s.currentOtherBits << ",\"reference_container_bytes\":" << baseline.data.size() + 168
                         << ",\"current_container_bytes\":" << packet.data.size() + 168 << "}\n";
                }
            }
            const auto frame = codec->decode(packet, source.width, source.height);
            writePng(prefix.wstring() + L".png", frame);
            std::string legend;
            writePng(prefix.wstring() + L"-flat.png", budgetDebugImage(*o.budgetHeatmap, 20, legend));
            writePng(prefix.wstring() + L"-weights.png", budgetDebugImage(*o.budgetHeatmap, 17, legend));
            writePng(prefix.wstring() + L"-bits.png", budgetDebugImage(*o.budgetHeatmap, 16, legend));
            writePng(prefix.wstring() + L"-routing.png", budgetDebugImage(*o.budgetHeatmap, 21, legend));
            auto inspection = inspectPacket(packet, source.width, source.height, {20, 0}, 1, 0, o.budgetHeatmap.get());
            writePng(prefix.wstring() + L"-debug.png", inspection.comparison);
            std::ofstream csv(prefix.wstring() + L"-allocation.csv");
            require(bool(csv), "Cannot write allocation report.");
            csv << std::setprecision(12);
            csv << "cell_x,cell_y,flat_eligibility,y_penalty,c1_penalty,c2_penalty,detail_bits,y_step,c1_step,c2_step\n";
            const auto& m = *o.budgetHeatmap;
            std::ofstream tiles(prefix.wstring() + L"-tiles.csv");
            require(bool(tiles), "Cannot write band tile costs.");
            tiles << std::setprecision(12);
            tiles << "x,y,width,height,channel,level,detail_bits,penalty,quantizer\n";
            for (const auto& tile : m.tiles)
                tiles << tile.x << ',' << tile.y << ',' << tile.width << ',' << tile.height << ','
                      << tile.channel << ',' << tile.level << ',' << tile.bits << ',' << tile.penalty << ',' << tile.quantizer << '\n';
            std::ofstream metadata(prefix.wstring() + L"-allocation.json");
            require(bool(metadata), "Cannot write allocation geometry.");
            metadata << std::setprecision(12) << "{\"source_width\":" << source.width << ",\"source_height\":" << source.height
                     << ",\"working_width\":" << m.width << ",\"working_height\":" << m.height << ",\"cell\":" << m.cell
                     << ",\"packet_bytes\":" << packet.data.size() << ",\"flat_allocation\":" << o.flatAllocation
                     << ",\"flat_threshold\":" << o.flatThreshold << ",\"flat_savings\":" << o.flatSavings << ",\"aq_enabled\":" << o.adaptiveQuantizationEnabled
                     << ",\"mapped_detail_bytes\":" << m.mappedBytes << ",\"range_detail_bytes\":" << m.detailBytes
                     << ",\"unmapped_padding_bytes\":" << m.unmappedDetailBytes
                     << "}\n";

            for (size_t i = 0; i < m.bits.size(); ++i)
                csv << i % m.cols << ',' << i / m.cols << ',' << m.flatCoverage[i] << ',' << m.penalties[0][i] << ','
                    << m.penalties[1][i] << ',' << m.penalties[2][i] << ',' << m.bits[i] << ',' << m.localQuantSteps[0][i] << ',' << m.localQuantSteps[1][i] << ',' << m.localQuantSteps[2][i] << '\n';
            SequenceInfo info{source.width, source.height, 1, 1, CodecId::Fajk, 1, 0, 0};
            SequenceWriter writer(prefix.wstring() + L".fajk", info, true);
            writer.append(packet);
            writer.finish();
            std::cout << "Allocation " << o.flatAllocation << "%: " << packet.data.size()
                      << " packet bytes; mapped detail bits Y/C1/C2 " << m.channelBits[0] << '/' << m.channelBits[1]
                      << '/' << m.channelBits[2] << '\n';
        } else if (command == L"measure") {
            require(argc >= 6 && argc <= 18,
                    "measure INPUT PREFIX QUALITY REPEATS [420|422|444] [WORKERS] [DEBLOCKING] "
                    "[wavelet] [TARGET_BYTES] [SHARPENING] [r590|r590-buffer] [FLAT_ALLOCATION_0_100] [AQ_ENABLED_0_1] [FLAT_SAVINGS_0_100] [FLAT_THRESHOLD_0_100] [RECONSTRUCTION_INI]");
            auto source = readImage(argv[2]);
            Path prefix = argv[3];
            EncodeOptions o;
            o.backend=backend;
            o.speed=speed;
            o.quality = float(integer(argv[4]));
            const auto count = integer(argv[5]);
            require(count > 0 && count <= 10000, "Invalid repeat count.");
            const auto chroma = argc >= 7 ? integer(argv[6]) : 420u;
            require(chroma == 420 || chroma == 422 || chroma == 444, "Sampling must be 420, 422 or 444.");
            o.chroma420 = chroma == 420;
            o.chroma422 = chroma == 422;
            o.threads = argc >= 8 ? integer(argv[7]) : 0;
            const auto deblock = argc >= 9 ? integer(argv[8]) : 100, sharpen = argc >= 12 ? integer(argv[11]) : 0;
            require(argc < 10 || std::wstring(argv[9]) == L"wavelet", "Only current wavelet is supported.");
            o.targetBytes = argc >= 11 ? integer(argv[10]) : 0;
            std::wstring profile = argc >= 13 ? argv[12] : L"r600";
            // R500 keeps the established independent-frame measurement paths.
            if(profile.rfind(L"r600",0)==0)profile.replace(0,4,L"r490");
            if(profile.rfind(L"r590",0)==0)profile.replace(0,4,L"r490");
            if(profile.rfind(L"r580",0)==0)profile.replace(0,4,L"r490");
            if(profile.rfind(L"r570",0)==0)profile.replace(0,4,L"r490");
            if(profile.rfind(L"r560",0)==0)profile.replace(0,4,L"r490");
            if(profile.rfind(L"r550",0)==0)profile.replace(0,4,L"r490");
            if(profile.rfind(L"r520",0)==0)profile.replace(0,4,L"r490");
            if(profile.rfind(L"r510",0)==0)profile.replace(0,4,L"r490");
            if(profile.rfind(L"r500",0)==0)profile.replace(0,4,L"r490");
            require(profile == L"r441" || profile == L"r441-buffer" || profile == L"r450" ||
                        profile == L"r450-buffer" || profile == L"r460" || profile == L"r460-buffer" ||
                        profile == L"r461" || profile == L"r461-buffer" || profile == L"r462" ||
                        profile == L"r462-buffer" || profile == L"r464" || profile == L"r464-buffer" || profile == L"r465" || profile == L"r465-buffer" || profile == L"r470" || profile == L"r470-buffer" || profile == L"r470-gpu" || profile == L"r470-gpu-buffer" || profile == L"r480" || profile == L"r490" || profile == L"r480-buffer" || profile == L"r490-buffer" || profile == L"r480-gpu" || profile == L"r490-gpu" || profile == L"r480-gpu-buffer" || profile == L"r490-gpu-buffer",
                    "Only the current R600.4 / R500 / R470 / R441 packet family is supported.");
            if(profile==L"r470-gpu"||profile==L"r470-gpu-buffer"||profile==L"r480-gpu"||profile==L"r490-gpu"||profile==L"r480-gpu-buffer"||profile==L"r490-gpu-buffer"){backend=ComputeBackend::Gpu;o.backend=backend;}
            o.flatAllocation = argc >= 14 ? integer(argv[13]) : 50;
            require(o.flatAllocation <= 100, "Flat allocation must be 0-100.");
            const auto aqEnabled = argc >= 15 ? integer(argv[14]) : 0;
            require(aqEnabled <= 1, "AQ enabled must be 0 or 1.");
            o.adaptiveQuantizationEnabled = aqEnabled != 0;
            o.flatThreshold = argc >= 17 ? integer(argv[16]) : 0;
            require(o.flatThreshold <= 100,"Flat threshold must be 0-100.");
            o.flatSavings = argc >= 16 ? integer(argv[15]) : 0;
            require(o.flatSavings <= 100, "Flat savings must be 0-100.");
            if (argc >= 18) {
                const auto ini=std::filesystem::absolute(Path(argv[17])).wstring();
                require(std::filesystem::is_regular_file(ini),"Missing benchmark reconstruction settings.");
                auto read=[&](const wchar_t* name,uint32_t value){return GetPrivateProfileIntW(L"Studio",name,value,ini.c_str());};
                o.reconstruction441.intensity=read(L"r441Intensity",o.reconstruction441.intensity);
                o.reconstruction441.strongerTangents=read(L"r441Tangents",o.reconstruction441.strongerTangents);
                o.reconstruction441.antialias=read(L"r441AA",o.reconstruction441.antialias);
                o.reconstruction441.contrast=read(L"r441Contrast",o.reconstruction441.contrast);
                o.reconstruction441.style=read(L"r441Style",o.reconstruction441.style);
                o.reconstruction441.protectText=read(L"r441TextProtection",o.reconstruction441.protectText)!=0;
                o.shapeIntensity=read(L"shapeIntensity",o.shapeIntensity);
                o.contourIntensity=read(L"contourIntensity",o.contourIntensity);
            }
            auto codec = makeCodec(CodecId::Fajk);
            codec->setComputeBackend(backend);
            codec->setThreads(o.threads);
            codec->setDeblocking(deblock);
            codec->setSharpening(sharpen);
            std::ofstream csv(prefix.wstring() + L".csv"), stages(prefix.wstring() + L"-stages.csv");
            require(bool(csv) && bool(stages), "Cannot write timings.");
            csv << "quality,repeat,bytes,encode_ms,decode_ms,encode_cpu_ms,decode_cpu_ms,encode_cycles,decode_cycles,gpu_transforms,gpu_resamples,gpu_uploaded_bytes,gpu_downloaded_bytes,gpu_fallbacks,gpu_contour_filters\n";
            stages << "repeat,stage,ms\n";
            Frame image;
            const bool directBuffer = profile == L"r441-buffer" || profile == L"r450-buffer" ||
                                      profile == L"r460-buffer" || profile == L"r461-buffer" ||
                                      profile == L"r462-buffer" || profile == L"r464-buffer" || profile == L"r465-buffer" || profile == L"r470-buffer" || profile == L"r470-gpu-buffer" || profile == L"r480-buffer" || profile == L"r490-buffer" || profile == L"r480-gpu-buffer" || profile == L"r490-gpu-buffer";
            if (directBuffer)
                image = {source.width, source.height, source.pts, source.duration, Bytes(source.pixels.size())};
            Packet packet;
            for (int repeat = -2; repeat < int(count); ++repeat) {
                std::vector<TimingEvent> events;
                TimingCapture capture(&events);
                auto start = std::chrono::steady_clock::now();
                const auto workStart=cpuWork();const auto gpuStart=gpuActivity();
                packet = codec->encode(source, o);
                const auto enc = elapsed(start);const auto workEncoded=cpuWork();
                start = std::chrono::steady_clock::now();
                if (directBuffer) {
                    decodeFrameToBuffer(packet, source.width, source.height, o.threads, deblock, image.pixels, sharpen,
                                        true, backend);
                } else
                    image = codec->decode(packet, source.width, source.height);
                const auto dec = elapsed(start);
                if (repeat >= 0) {
                    const auto workDecoded=cpuWork();const auto gpu=gpuActivity();
                    csv << o.quality << ',' << repeat << ',' << packet.data.size() << ',' << enc << ',' << dec
                        << ',' << (workEncoded.time-workStart.time)/10000. << ',' << (workDecoded.time-workEncoded.time)/10000.
                        << ',' << workEncoded.cycles-workStart.cycles << ',' << workDecoded.cycles-workEncoded.cycles
                        << ',' << gpu.transforms-gpuStart.transforms << ',' << gpu.resamples-gpuStart.resamples
                        << ',' << gpu.uploadedBytes-gpuStart.uploadedBytes << ',' << gpu.downloadedBytes-gpuStart.downloadedBytes << ',' << gpu.fallbacks-gpuStart.fallbacks << ',' << gpu.contourFilters-gpuStart.contourFilters << '\n';
                    for (auto event : events)
                        stages << repeat << ',' << event.stage << ',' << event.ms << '\n';
                }
            }
            writePng(prefix.wstring() + L".png", image);
            SequenceInfo info{source.width, source.height, 30, 1, CodecId::Fajk, 1, 0, 0};
            SequenceWriter writer(prefix.wstring() + L".fajkv", info, true);
            writer.append(packet);
            writer.finish();
        } else if (command == L"--help" || command == L"help")
            help();
        else
            throw std::runtime_error("Unknown or retired command.");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
