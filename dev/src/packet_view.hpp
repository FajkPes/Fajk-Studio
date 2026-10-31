#pragma once
#include "fajk/types.hpp"

namespace fajk {
// Borrowed for one synchronous decode, including all joined worker tasks.
// The source packet must remain alive; no decoder may retain this view.
struct PacketView {
    std::span<const uint8_t> data;
    int64_t pts = 0, duration = 0;
    std::shared_ptr<const Packet> reference;
    std::shared_ptr<const Packet> keyIdentity;
    bool cachePresentation=false;
    PacketView(const Packet& packet) : data(packet.data), pts(packet.pts), duration(packet.duration), reference(packet.reference),keyIdentity(packet.keyIdentity),cachePresentation(packet.cachePresentation) {}
    PacketView(std::span<const uint8_t> bytes, int64_t timestamp = 0, int64_t length = 0)
        : data(bytes), pts(timestamp), duration(length) {}
};
} // namespace fajk
