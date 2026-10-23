#pragma once
#include "fajk/types.hpp"
#include <windows.h>
#include <atomic>

namespace fajk {
// Check aliases before opening any output, including hard links and differently
// cased Windows paths. weakly_canonical also resolves existing parent links.
inline void requireDifferentFiles(const Path& input,const Path& output){
    std::error_code error;
    const bool same=std::filesystem::equivalent(input,output,error);
    require(error||!same,"Output must not overwrite the input.");
    auto normalized=[&](const Path& path){
        auto result=std::filesystem::weakly_canonical(path,error);
        return (error?std::filesystem::absolute(path).lexically_normal():result).wstring();
    };
    auto a=normalized(input),b=normalized(output);
    require(CompareStringOrdinal(a.c_str(),int(a.size()),b.c_str(),int(b.size()),TRUE)!=CSTR_EQUAL,
            "Output must not overwrite the input.");
}

// Diagnostics must have the same failure behavior as encoded files: write a
// sibling temporary, flush it, then commit without truncating the destination.
inline void writeAtomicText(const Path& destination,const std::string& text,bool replace){
    require(replace||!std::filesystem::exists(destination),"Output already exists.");
    static std::atomic_uint64_t counter{0};
    Path temporary=destination;
    temporary+=L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L"."+std::to_wstring(++counter)+L".partial";
    HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Cannot create diagnostic output.");
    struct Cleanup {
        HANDLE handle;Path temporary;
        ~Cleanup(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);std::error_code error;std::filesystem::remove(temporary,error);}
    } cleanup{handle,temporary};
    size_t position=0;
    while(position<text.size()){
        DWORD written=0;
        const auto count=DWORD(std::min<size_t>(text.size()-position,1u<<20));
        require(WriteFile(handle,text.data()+position,count,&written,nullptr)&&written>0,"Cannot write diagnostic output.");
        position+=written;
    }
    require(FlushFileBuffers(handle)!=FALSE,"Cannot flush diagnostic output.");
    const bool closed=CloseHandle(handle)!=FALSE;cleanup.handle=INVALID_HANDLE_VALUE;
    require(closed,"Cannot close diagnostic output.");
    require(MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))!=FALSE,
            "Cannot commit diagnostic output. Existing destination was preserved.");
}
}
