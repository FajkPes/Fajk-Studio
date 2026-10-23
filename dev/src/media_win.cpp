#include "fajk/media.hpp"
#include "fajk/codec.hpp"
#include <windows.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <emmintrin.h>
#include <atomic>
namespace fajk {
using Microsoft::WRL::ComPtr;
namespace {
void hr(HRESULT value,const char* what){if(FAILED(value)){std::ostringstream s;s<<what<<" (0x"<<std::hex<<uint32_t(value)<<").";throw std::runtime_error(s.str());}}
struct Apartment {
    HRESULT status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Apartment(){if(status!=RPC_E_CHANGED_MODE)hr(status,"COM initialization failed");}
    ~Apartment(){if(SUCCEEDED(status))CoUninitialize();}
};
ComPtr<IWICImagingFactory> imaging(){ComPtr<IWICImagingFactory> f;hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"WIC unavailable");return f;}
bool imageExtension(const Path& p){auto s=p.extension().wstring();std::transform(s.begin(),s.end(),s.begin(),::towlower);return s==L".png"||s==L".jpg"||s==L".jpeg"||s==L".bmp"||s==L".tif"||s==L".tiff";}
}
MediaRuntime::MediaRuntime(){hr(MFStartup(MF_VERSION),"Media Foundation unavailable");}
MediaRuntime::~MediaRuntime(){MFShutdown();}
Frame readImage(const Path& path){
    Apartment apartment;auto factory=imaging();ComPtr<IWICBitmapDecoder> decoder;
    hr(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"Image cannot be opened");
    ComPtr<IWICBitmapFrameDecode> bitmap;hr(decoder->GetFrame(0,&bitmap),"Image frame cannot be read");
    Frame f;hr(bitmap->GetSize(&f.width,&f.height),"Invalid image dimensions");f.pixels.resize(pixelBytes(f.width,f.height));f.duration=10000000;
    ComPtr<IWICFormatConverter> convert;hr(factory->CreateFormatConverter(&convert),"WIC converter unavailable");
    hr(convert->Initialize(bitmap.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Image pixel conversion failed");
    hr(convert->CopyPixels(nullptr,f.width*4,UINT(f.pixels.size()),f.pixels.data()),"Image pixels cannot be read");return f;
}
static void writePngData(const Path& path,const Frame& frame){
    frame.validate();Apartment apartment;auto factory=imaging();ComPtr<IWICStream> stream;
    hr(factory->CreateStream(&stream),"PNG stream unavailable");hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"Cannot create PNG");
    ComPtr<IWICBitmapEncoder> encoder;hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder unavailable");
    hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG initialization failed");
    ComPtr<IWICBitmapFrameEncode> f;hr(encoder->CreateNewFrame(&f,nullptr),"PNG frame failed");hr(f->Initialize(nullptr),"PNG frame initialization failed");
    hr(f->SetSize(frame.width,frame.height),"PNG size failed");auto format=GUID_WICPixelFormat32bppBGRA;hr(f->SetPixelFormat(&format),"PNG format failed");
    // WIC PNG negotiates BGRA, not RGBA. Convert one scanline, preserving alpha.
    require(format==GUID_WICPixelFormat32bppBGRA,"PNG BGRA unavailable.");Bytes row(size_t(frame.width)*4);
    for(uint32_t y=0;y<frame.height;++y){const auto* source=frame.pixels.data()+size_t(y)*row.size();
        for(uint32_t x=0;x<frame.width;++x){row[4*x]=source[4*x+2];row[4*x+1]=source[4*x+1];row[4*x+2]=source[4*x];row[4*x+3]=source[4*x+3];}
        hr(f->WritePixels(1,frame.width*4,UINT(row.size()),row.data()),"PNG write failed");}
    hr(f->Commit(),"PNG frame commit failed");hr(encoder->Commit(),"PNG commit failed");
}
void writePng(const Path& path,const Frame& frame){
    frame.validate();static std::atomic<uint64_t> sequence{0};
    Path temporary=path;temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L"."+std::to_wstring(++sequence)+L".partial";
    HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    hr(handle==INVALID_HANDLE_VALUE?HRESULT_FROM_WIN32(GetLastError()):S_OK,"Cannot create temporary PNG");CloseHandle(handle);
    struct Cleanup{Path path;~Cleanup(){std::error_code ec;std::filesystem::remove(path,ec);}} cleanup{temporary};
    writePngData(temporary,frame);
    hr(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)?S_OK:HRESULT_FROM_WIN32(GetLastError()),"Cannot save completed PNG");
}
struct MediaSource::Impl {
    Apartment apartment;MediaInfo info;ComPtr<IMFSourceReader> reader;Frame image;Path imagePath;bool delivered=false;LONG stride=0;int64_t lastPts=-1;
    explicit Impl(const Path& path,uint32_t maxWidth){
        if(imageExtension(path)){image=readImage(path);imagePath=path;info={image.width,image.height,1,1,10000000,true};require(!maxWidth||maxWidth>=image.width,"Image resizing is not supported; use original width.");return;}
        ComPtr<IMFAttributes> attrs;hr(MFCreateAttributes(&attrs,3),"Media attributes failed");
        hr(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE),"Video processing setup failed");
        hr(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE),"Hardware transform setup failed");
        hr(MFCreateSourceReaderFromURL(path.c_str(),attrs.Get(),&reader),"Video cannot be opened; install the source codec supported by Windows");
        hr(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE),"Stream setup failed");
        hr(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE),"Video stream missing");
        ComPtr<IMFMediaType> native;hr(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,&native),"Video metadata missing");
        hr(MFGetAttributeSize(native.Get(),MF_MT_FRAME_SIZE,&info.width,&info.height),"Video dimensions missing");pixelBytes(info.width,info.height);
        MFGetAttributeRatio(native.Get(),MF_MT_FRAME_RATE,&info.fpsNum,&info.fpsDen);if(!info.fpsNum||!info.fpsDen){info.fpsNum=30;info.fpsDen=1;}
        UINT32 transfer=0;native->GetUINT32(MF_MT_TRANSFER_FUNCTION,&transfer);
        require(transfer!=MFVideoTransFunc_2084&&transfer!=MFVideoTransFunc_HLG,"HDR input needs explicit tone mapping; convert it to SDR before importing.");
        PROPVARIANT duration;PropVariantInit(&duration);if(SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&duration))&&duration.vt==VT_UI8&&duration.uhVal.QuadPart<=INT64_MAX)info.duration=int64_t(duration.uhVal.QuadPart);PropVariantClear(&duration);
        ComPtr<IMFMediaType> target;hr(MFCreateMediaType(&target),"Video type failed");hr(target->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"Video type failed");hr(target->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32),"RGB32 unavailable");
        if(maxWidth&&maxWidth<info.width){uint32_t outHeight=std::max(2u,uint32_t(uint64_t(info.height)*maxWidth/info.width)&~1u);hr(MFSetAttributeSize(target.Get(),MF_MT_FRAME_SIZE,maxWidth&~1u,outHeight),"Video resizing failed");}
        hr(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,target.Get()),"Video decoder cannot output RGB32");
        updateType();
    }
    void updateType(){ComPtr<IMFMediaType> type;hr(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,&type),"Video type missing");UINT32 w=0,h=0;hr(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h),"Video dimensions missing");pixelBytes(w,h);info.width=w;info.height=h;
        UINT32 rawStride=0;if(SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE,&rawStride)))stride=LONG(rawStride);else hr(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,w,&stride),"Video stride missing");
    }
    bool next(Frame& f){
        if(info.image){if(delivered)return false;f=std::move(image);delivered=true;return true;}
        while(true){DWORD flags=0;LONGLONG pts=0;ComPtr<IMFSample> sample;
            hr(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,&pts,&sample),"Video sample decoding failed");
            if(flags&MF_SOURCE_READERF_ERROR)throw std::runtime_error("Source decoder reported an error.");
            if(flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED){auto w=info.width,h=info.height;updateType();require(w==info.width&&h==info.height,"Variable video dimensions are unsupported.");}
            if(!sample){if(flags&MF_SOURCE_READERF_ENDOFSTREAM)return false;continue;}
            f.width=info.width;f.height=info.height;f.pts=std::max<int64_t>(0,pts);f.duration=0;f.pixels.resize(pixelBytes(info.width,info.height));
            require(lastPts<0||f.pts>lastPts,"Non-monotonic input timestamps.");lastPts=f.pts;
            if(FAILED(sample->GetSampleDuration(&f.duration))||f.duration<=0)f.duration=std::max<int64_t>(1,10000000ll*info.fpsDen/info.fpsNum);
            require(f.pts<=INT64_MAX-f.duration,"Input timestamp exceeds the supported time range.");
            ComPtr<IMFMediaBuffer> buffer;DWORD buffers=0;hr(sample->GetBufferCount(&buffers),"Video buffer count failed");
            if(buffers==1)hr(sample->GetBufferByIndex(0,&buffer),"Video buffer failed");else hr(sample->ConvertToContiguousBuffer(&buffer),"Video buffer failed");
            ComPtr<IMF2DBuffer> buffer2;BYTE* scan=nullptr;LONG pitch=stride;BYTE* base=nullptr;DWORD length=0;bool is2d=SUCCEEDED(buffer.As(&buffer2));
            if(is2d)hr(buffer2->Lock2D(&scan,&pitch),"Video surface lock failed");
            else{hr(buffer->Lock(&base,nullptr,&length),"Video buffer lock failed");scan=base;}
            struct Unlock {IMFMediaBuffer* b;IMF2DBuffer* d;~Unlock(){if(d)d->Unlock2D();else b->Unlock();}} unlock{buffer.Get(),is2d?buffer2.Get():nullptr};
            require(std::abs(int64_t(pitch))>=int64_t(info.width)*4,"Invalid video stride.");
            if(!is2d)require(uint64_t(length)>=uint64_t(std::abs(int64_t(pitch)))*info.height,"Video buffer is truncated.");
            if(!is2d&&pitch<0)scan+=size_t(info.height-1)*size_t(-int64_t(pitch));
            for(uint32_t y=0;y<info.height;++y){const auto* src=scan+ptrdiff_t(y)*pitch;auto* dst=f.pixels.data()+size_t(y)*info.width*4;
                uint32_t x=0;const auto rb=_mm_set1_epi32(255),green=_mm_set1_epi32(0x0000ff00),alpha=_mm_set1_epi32(int(0xff000000u));
                for(;x+4<=info.width;x+=4){auto v=_mm_loadu_si128(reinterpret_cast<const __m128i*>(src+4*x));auto swapped=_mm_or_si128(alpha,_mm_or_si128(_mm_and_si128(v,green),_mm_or_si128(_mm_slli_epi32(_mm_and_si128(v,rb),16),_mm_and_si128(_mm_srli_epi32(v,16),rb))));_mm_storeu_si128(reinterpret_cast<__m128i*>(dst+4*x),swapped);}
                for(;x<info.width;++x){dst[4*x]=src[4*x+2];dst[4*x+1]=src[4*x+1];dst[4*x+2]=src[4*x];dst[4*x+3]=255;}}
            return true;
        }
    }
};
MediaSource::MediaSource(const Path& path,uint32_t maxWidth):impl_(std::make_unique<Impl>(path,maxWidth)){}
MediaSource::~MediaSource()=default;
const MediaInfo& MediaSource::info()const{return impl_->info;}
bool MediaSource::next(Frame& f){return impl_->next(f);}
void MediaSource::seek(int64_t pts){if(impl_->info.image){if(impl_->image.pixels.empty()){auto image=readImage(impl_->imagePath);require(image.width==impl_->info.width&&image.height==impl_->info.height,"Image dimensions changed since opening.");impl_->image=std::move(image);}impl_->delivered=false;return;}PROPVARIANT v;PropVariantInit(&v);v.vt=VT_I8;v.hVal.QuadPart=std::max<int64_t>(0,pts);hr(impl_->reader->SetCurrentPosition(GUID_NULL,v),"Video seeking failed");impl_->lastPts=-1;}
}
