#pragma once
#include "types.hpp"
#include <fstream>
#include <memory>

namespace fajk {
uint32_t crc32(std::span<const uint8_t> bytes);
struct IndexEntry {
    uint64_t offset = 0, size = 0;
    int64_t pts = 0, duration = 0;
    uint32_t checksum = 0;
    uint32_t reference = 0; // 0: independent; otherwise keyframe index + 1.
};
// Single owner, sequential writer. Only encoded packets are ever written.
class SequenceWriter {
public:
    SequenceWriter(const Path& destination, SequenceInfo info, bool replace = false);
    ~SequenceWriter();
    SequenceWriter(const SequenceWriter&) = delete;
    SequenceWriter& operator=(const SequenceWriter&) = delete;
    void append(const Packet& packet);
    void finish();
    uint64_t bytesWritten() const { return position_; }
private:
    Path destination_, temporary_;
    SequenceInfo info_;
    std::fstream file_;
    std::vector<IndexEntry> index_;
    uint64_t position_ = 128;
    bool committed_ = false, replace_ = false, stillImage_ = false;
};
// Index in RAM, payload on disk. Skipping never reads/decodes skipped packets.
// One reader per decoding worker: the stream position is not shared.
class SequenceReader {
public:
    explicit SequenceReader(const Path& path);
    const SequenceInfo& info() const { return info_; }
    const std::vector<IndexEntry>& index() const { return index_; }
    Packet read(uint64_t frame);
    uint64_t frameAt(int64_t pts) const;
private:
    std::ifstream file_;
    SequenceInfo info_;
    std::vector<IndexEntry> index_;
    uint64_t cachedReferenceIndex_ = UINT64_MAX;
    std::shared_ptr<const Packet> cachedReference_;
};
}
