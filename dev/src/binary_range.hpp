#pragma once
#include "fajk/entropy.hpp"

namespace fajk::binaryRange {
// Bitstream constants shared by spatial coefficients and screen bitmaps.
// Changing these requires a new wire format, not an encoder tuning change.
inline constexpr unsigned probabilityBits = 12;
inline constexpr uint32_t probabilityScale = 1u << probabilityBits;
inline constexpr uint32_t normalizationThreshold = 1u << 24;

struct Encoder {
    Bytes bytes;
    uint64_t low = 0;
    uint32_t range = ~0u;
    uint8_t cache = 0;
    size_t pending = 1;
    unsigned adaptation = 5;

    void shift() {
        const auto lower = uint32_t(low), carry = uint32_t(low >> 32);
        if (lower < 0xff000000u || carry) {
            auto value = cache;
            do {
                bytes.push_back(uint8_t(value + carry));
                value = 255;
            } while (--pending);
            cache = uint8_t(lower >> 24);
        }
        ++pending;
        low = uint32_t(lower << 8);
    }
    void bit(uint16_t& probability, bool one) {
        const uint32_t bound = (range >> probabilityBits) * probability;
        if (one) {
            low += bound;
            range -= bound;
            probability -= probability >> adaptation;
        } else {
            range = bound;
            probability += (probabilityScale - probability) >> adaptation;
        }
        if (range < normalizationThreshold) {
            range <<= 8;
            shift();
        }
    }
    void raw(bool one) {
        const auto bound = range >> 1;
        if (one) {
            low += bound;
            range -= bound;
        } else
            range = bound;
        if (range < normalizationThreshold) {
            range <<= 8;
            shift();
        }
    }
    double positionBits() const { return double(bytes.size() + pending - 1) * 8 + std::log2(double(~0u) / range); }
    Bytes finish() {
        for (int i = 0; i < 5; ++i)
            shift();
        return std::move(bytes);
    }
};

struct Decoder {
    ByteReader input;
    uint32_t code = 0, range = ~0u;
    unsigned adaptation;
    explicit Decoder(std::span<const uint8_t> bytes, uint8_t prefix = 0, unsigned shift = 5)
        : input{bytes}, adaptation(shift) {
        require(input.u8() == prefix, "Invalid binary range prefix.");
        for (int i = 0; i < 4; ++i)
            code = (code << 8) | input.u8();
    }
    bool bit(uint16_t& probability) {
        const uint32_t bound = (range >> probabilityBits) * probability;
        const bool one = code >= bound;
        if (one) {
            code -= bound;
            range -= bound;
            probability -= probability >> adaptation;
        } else {
            range = bound;
            probability += (probabilityScale - probability) >> adaptation;
        }
        if (range < normalizationThreshold) {
            range <<= 8;
            code = (code << 8) | input.u8();
        }
        return one;
    }
    bool raw() {
        const auto bound = range >> 1;
        const bool one = code >= bound;
        if (one) {
            code -= bound;
            range -= bound;
        } else
            range = bound;
        if (range < normalizationThreshold) {
            range <<= 8;
            code = (code << 8) | input.u8();
        }
        return one;
    }
};
} // namespace fajk::binaryRange
