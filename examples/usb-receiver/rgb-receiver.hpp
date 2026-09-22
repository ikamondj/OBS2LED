#pragma once

// Portable C++11 streaming decoder. No allocation, Arduino dependency, or LED driver.
#include <stddef.h>
#include <stdint.h>

template<size_t Capacity>
class RgbReceiver {
public:
    void reset() { header_used_ = 0; payload_used_ = 0; payload_size_ = 0; crc_ = 0xffffffffu; }

    // on_frame(width, height, sequence, rgb) runs only for a complete, valid frame.
    // rgb remains valid until the next feed(); copy/swap it if rendering asynchronously.
    template<class OnFrame>
    void feed(uint8_t byte, OnFrame on_frame)
    {
        static const uint8_t magic[4] = {'O', '2', 'L', 'F'};
        if (payload_size_) {
            pixels_[payload_used_++] = byte;
            crc_ ^= byte;
            for (int bit = 0; bit < 8; ++bit)
                crc_ = (crc_ >> 1) ^ (0xedb88320u & (0u - (crc_ & 1u)));
            if (payload_used_ == payload_size_) {
                if (~crc_ == little(20, 4)) on_frame(little(8, 2), little(10, 2), little(12, 4), pixels_);
                reset();
            }
            return;
        }
        if (header_used_ < 4 && byte != magic[header_used_]) {
            header_used_ = byte == magic[0] ? 1 : 0;
            if (header_used_) header_[0] = byte;
            return;
        }
        header_[header_used_++] = byte;
        if (header_used_ != sizeof(header_)) return;
        const uint32_t width = little(8, 2), height = little(10, 2), size = little(16, 4);
        if (header_[4] == 1 && header_[5] == 0 && little(6, 2) == 24 && width && height &&
            width <= 512 && height <= 512 && size == width * height * 3 && size <= Capacity) {
            payload_size_ = size;
            return;
        }
        // A malformed candidate may contain the beginning of the next header.
        uint8_t tail[23];
        for (size_t i = 0; i < sizeof(tail); ++i) tail[i] = header_[i + 1];
        reset();
        for (size_t i = 0; i < sizeof(tail); ++i) feed(tail[i], on_frame);
    }
private:
    uint32_t little(size_t at, size_t count) const
    {
        uint32_t value = 0;
        for (size_t i = 0; i < count; ++i) value |= uint32_t(header_[at + i]) << (8 * i);
        return value;
    }
    uint8_t header_[24]{};
    uint8_t pixels_[Capacity]{};
    size_t header_used_ = 0, payload_used_ = 0, payload_size_ = 0;
    uint32_t crc_ = 0xffffffffu;
};
