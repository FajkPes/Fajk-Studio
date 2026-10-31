#pragma once
#include "fajk/types.hpp"

namespace fajk {
// A serialized decoder owns its reference state independently of the worker
// thread running it. Only the binding is thread-local, never the owned cache.
class DecodeSession {
    struct State {virtual ~State()=default;virtual size_t bytes()const noexcept=0;};
    template<class T>struct Value final:State {T value;size_t bytes()const noexcept override{if constexpr(requires{value.retainedBytes();})return value.retainedBytes();else return 0;}};
    struct Entry {const void* key;std::unique_ptr<State> state;uint64_t touched;};
    std::vector<Entry> states_;
    size_t budget_=256u*1024u*1024u;uint64_t clock_=0;
public:
    DecodeSession()=default;
    DecodeSession(const DecodeSession&)=delete;
    DecodeSession& operator=(const DecodeSession&)=delete;
    void setCacheBudget(size_t bytes){budget_=bytes;trim();}
    size_t cacheBudget()const noexcept{return budget_;}
    size_t retainedBytes()const noexcept{size_t result=0;for(const auto& entry:states_)result+=entry.state->bytes();return result;}
    void trim()noexcept{
        while(retainedBytes()>budget_){
            auto oldest=states_.end();for(auto it=states_.begin();it!=states_.end();++it)if(it->state->bytes()&&(oldest==states_.end()||it->touched<oldest->touched))oldest=it;
            if(oldest==states_.end())break;states_.erase(oldest);
        }
    }
    template<class T>T& state(){
        static char key=0;
        for(auto& entry:states_)if(entry.key==&key){entry.touched=++clock_;return static_cast<Value<T>&>(*entry.state).value;}
        auto value=std::make_unique<Value<T>>();auto* result=&value->value;
        states_.push_back({&key,std::move(value),++clock_});return *result;
    }
};
inline thread_local DecodeSession* activeDecodeSession=nullptr;
class ScopedDecodeSession {
    DecodeSession* previous_;
public:
    explicit ScopedDecodeSession(DecodeSession& session):previous_(activeDecodeSession){activeDecodeSession=&session;}
    ~ScopedDecodeSession(){auto* current=activeDecodeSession;activeDecodeSession=previous_;if(current&&current!=previous_)current->trim();}
    ScopedDecodeSession(const ScopedDecodeSession&)=delete;
    ScopedDecodeSession& operator=(const ScopedDecodeSession&)=delete;
};
template<class T>T& decodeState(){
    if(activeDecodeSession)return activeDecodeSession->state<T>();
    // Internal one-shot tools may call the low-level decoder without an owner.
    static thread_local T fallback;return fallback;
}
}
