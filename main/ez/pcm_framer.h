#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

// AFE frame sizes are independent of the wire format's 20ms frames.
class PcmFramer {
public:
    template <typename Sink>
    bool Push(const int16_t* samples, size_t count, Sink&& sink) {
        while (count) {
            auto take = std::min(count, frame_.size() - used_);
            std::copy_n(samples, take, frame_.data() + used_);
            used_ += take;
            samples += take;
            count -= take;
            if (used_ == frame_.size()) {
                if (!sink(frame_.data()))
                    return false;
                ++frames_;
                used_ = 0;
            }
        }
        return true;
    }
    template <typename Sink>
    bool Flush(Sink&& sink) {
        if (!used_)
            return true;
        std::fill(frame_.begin() + used_, frame_.end(), 0);
        if (!sink(frame_.data()))
            return false;
        ++frames_;
        used_ = 0;
        return true;
    }
    uint64_t Frames() const { return frames_; }

private:
    std::array<int16_t, 320> frame_{};
    size_t used_ = 0;
    uint64_t frames_ = 0;
};
