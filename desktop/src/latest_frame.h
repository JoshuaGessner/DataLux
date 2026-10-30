// One worker-owned active frame and one replaceable pending frame. Camera and decoder clocks
// remain separate: queue age uses steady_clock, never a subtraction from a sensor timestamp.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace fileflow::desktop::detail {

struct CameraFrame {
    std::vector<std::uint8_t> luma, u, v;
    int width = 0;
    int height = 0;
    std::uint64_t timestamp_ns = 0;
    bool full_resolution = false;
    std::chrono::steady_clock::time_point offered_at;
};

struct FrameDeliveryStats {
    std::uint64_t offered = 0;
    std::uint64_t accepted = 0;  // includes replacements
    std::uint64_t replaced = 0;
    std::uint64_t stale = 0;
    std::uint64_t invalid = 0;
    double copy_ms_sum = 0;
    double copy_ms_max = 0;
};

class LatestFrame {
  public:
    bool Offer(const std::vector<std::uint8_t>& luma, const std::vector<std::uint8_t>& u,
               const std::vector<std::uint8_t>& v, int width, int height,
               std::uint64_t timestamp_ns, bool full_resolution) {
        std::lock_guard lock(mu_);
        if (closed_) return false;
        ++stats_.offered;
        const bool valid_dimensions = width > 0 && height > 0 && static_cast<std::size_t>(width) <=
            std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(height);
        const auto area = valid_dimensions
                              ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height) : 0;
        const auto chroma_area = full_resolution ? area :
            (width > 1 && height > 1 ? static_cast<std::size_t>(width / 2) * static_cast<std::size_t>(height / 2) : 0);
        if (area == 0 || chroma_area == 0 || luma.size() != area || u.size() != chroma_area || v.size() != chroma_area ||
            timestamp_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            ++stats_.invalid;
            return false;
        }
        if (have_timestamp_ && timestamp_ns <= newest_timestamp_) {
            ++stats_.stale;
            return false;
        }
        // Copy once into reusable pending storage. Take swaps it with the worker's spare buffers;
        // offering another frame can never mutate the worker's active bytes.
        const auto copy_started = std::chrono::steady_clock::now();
        pending_.luma = luma;
        pending_.u = u;
        pending_.v = v;
        pending_.width = width;
        pending_.height = height;
        pending_.timestamp_ns = timestamp_ns;
        pending_.full_resolution = full_resolution;
        pending_.offered_at = std::chrono::steady_clock::now();
        const double copy_ms = std::chrono::duration<double, std::milli>(pending_.offered_at - copy_started).count();
        stats_.copy_ms_sum += copy_ms;
        if (copy_ms > stats_.copy_ms_max) stats_.copy_ms_max = copy_ms;
        if (ready_) ++stats_.replaced;
        ++stats_.accepted;
        newest_timestamp_ = timestamp_ns;
        have_timestamp_ = true;
        ready_ = true;
        cv_.notify_one();
        return true;
    }

    bool Take(CameraFrame* active) {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return closed_ || ready_; });
        if (closed_) return false;
        std::swap(*active, pending_);
        ready_ = false;  // consumed now; finishing this decode never clears a newer pending frame
        return true;
    }

    void Close() {
        std::lock_guard lock(mu_);
        closed_ = true;
        ready_ = false;
        cv_.notify_all();
    }

    [[nodiscard]] FrameDeliveryStats Stats() const {
        std::lock_guard lock(mu_);
        return stats_;
    }

  private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    CameraFrame pending_;
    FrameDeliveryStats stats_;
    std::uint64_t newest_timestamp_ = 0;
    bool have_timestamp_ = false;
    bool ready_ = false;
    bool closed_ = false;
};

}  // namespace fileflow::desktop::detail
