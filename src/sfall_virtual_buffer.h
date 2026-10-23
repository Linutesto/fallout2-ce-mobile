#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace fallout {

class SfallVirtualBuffer {
    static constexpr int kMaxSize = 10 * 1024 * 1024;
    std::vector<unsigned char> data_;
    int pos_ = 0;

public:
    explicit SfallVirtualBuffer(int size) {
        if (size >= 0 && size <= kMaxSize) {
            data_.assign(static_cast<size_t>(size), 0);
        }
    }

    explicit SfallVirtualBuffer(std::vector<unsigned char> bytes) {
        if (bytes.size() <= static_cast<size_t>(kMaxSize)) {
            data_ = std::move(bytes);
        }
    }

    int size() const { return static_cast<int>(data_.size()); }
    int pos() const { return pos_; }

    bool seek(int position) {
        if (position < 0 || position > size()) return false;
        pos_ = position;
        return true;
    }

    bool resize(int size) {
        if (size < 0 || size > kMaxSize) return false;
        data_.resize(static_cast<size_t>(size), 0);
        pos_ = 0;
        return true;
    }

    bool write(uint32_t value, int width) {
        if (width < 1 || width > 4) return false;
        if (pos_ + width > size()) return false;
        for (int i = width - 1; i >= 0; --i) {
            data_[static_cast<size_t>(pos_)] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
            ++pos_;
        }
        return true;
    }

    uint32_t read(int width) {
        if (width < 1 || width > 4) return 0;
        if (pos_ + width > size()) return 0;
        uint32_t value = 0;
        for (int i = 0; i < width; ++i) {
            value = (value << 8) | data_[static_cast<size_t>(pos_)];
            ++pos_;
        }
        return value;
    }

    const std::vector<unsigned char>& bytes() const { return data_; }
};

} // namespace fallout
