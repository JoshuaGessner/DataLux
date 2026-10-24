// Grayscale image views — the receiver's raw input.
//
// LOW-COPY BY CONSTRUCTION (ADR-0005). ImageView8 is a NON-OWNING, stride-aware view. That
// is not a stylistic choice: `YUV_420_888` hands us a Y plane whose row stride is decided by
// the driver and is frequently NOT equal to the width. Code that assumes stride == width
// works on one device and produces sheared garbage on the next.
//
// For M0/M1/M2 the Y plane alone is sufficient -- it is a full-resolution 8-bit luminance
// image, so we can ignore chroma entirely and dodge all chroma-subsampling loss. Colour
// (M3) needs a separate path; see docs/research/android-camera-pipeline.md.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fileflow {

class ImageView8 {
  public:
    ImageView8() = default;
    ImageView8(const std::uint8_t* data, int width, int height, std::ptrdiff_t stride)
        : data_(data), w_(width), h_(height), stride_(stride) {}

    [[nodiscard]] int width() const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }
    [[nodiscard]] std::ptrdiff_t stride() const noexcept { return stride_; }
    [[nodiscard]] bool empty() const noexcept { return data_ == nullptr || w_ <= 0 || h_ <= 0; }

    [[nodiscard]] std::uint8_t At(int x, int y) const noexcept {
        return data_[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_) +
                     static_cast<std::size_t>(x)];
    }

    // A rectangular window onto the same pixels, with NO COPY.
    //
    // This is the payoff of being stride-aware: a sub-rectangle is just a shifted base
    // pointer with the parent's stride retained. The tracker uses it to confine a search to
    // the neighbourhood of the previous detection, which is what turns per-frame cost from
    // O(image) into O(window).
    //
    // The requested rectangle is CLAMPED to the parent; an entirely out-of-bounds request
    // yields an empty view rather than a view onto foreign memory.
    [[nodiscard]] ImageView8 SubView(int x, int y, int w, int h) const noexcept {
        if (empty()) return {};
        const int x0 = x < 0 ? 0 : (x > w_ ? w_ : x);
        const int y0 = y < 0 ? 0 : (y > h_ ? h_ : y);
        int x1 = x + w;
        int y1 = y + h;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        if (x1 > w_) x1 = w_;
        if (y1 > h_) y1 = h_;
        const int sw = x1 - x0;
        const int sh = y1 - y0;
        if (sw <= 0 || sh <= 0) return {};
        return {data_ + static_cast<std::size_t>(y0) * static_cast<std::size_t>(stride_) +
                    static_cast<std::size_t>(x0),
                sw, sh, stride_};
    }

    // Bilinear sample in pixel coordinates, where (0,0) is the CENTRE of pixel [0][0].
    //
    // Returns NaN when the sample falls outside the image. NaN is the project-wide "no
    // usable sample" convention (see CapturedFrame::cell_samples) and the demodulator turns
    // it into an ERASURE rather than a guess -- an erasure costs the fountain layer one
    // symbol, a wrong guess costs an undetected bit error. Never clamp to the edge here:
    // clamping fabricates plausible data out of nothing.
    [[nodiscard]] double SampleBilinear(double x, double y) const noexcept {
        if (empty()) return std::nan("");
        if (!(x >= 0.0) || !(y >= 0.0)) return std::nan("");  // also rejects NaN input
        if (x > static_cast<double>(w_ - 1) || y > static_cast<double>(h_ - 1)) {
            return std::nan("");
        }
        const int x0 = static_cast<int>(x);
        const int y0 = static_cast<int>(y);
        const int x1 = x0 + 1 < w_ ? x0 + 1 : x0;
        const int y1 = y0 + 1 < h_ ? y0 + 1 : y0;
        const double fx = x - static_cast<double>(x0);
        const double fy = y - static_cast<double>(y0);

        const double top = static_cast<double>(At(x0, y0)) * (1.0 - fx) +
                           static_cast<double>(At(x1, y0)) * fx;
        const double bot = static_cast<double>(At(x0, y1)) * (1.0 - fx) +
                           static_cast<double>(At(x1, y1)) * fx;
        return top * (1.0 - fy) + bot * fy;
    }

    // [SampleBilinear] without its checks, for a caller that has already proved the point lies in
    // [0, width-1) x [0, height-1) -- the cell sampler does, once per cell for all its samples.
    [[nodiscard]] double SampleBilinearInside(double x, double y) const noexcept {
        const int x0 = static_cast<int>(x);
        const int y0 = static_cast<int>(y);
        const double fx = x - static_cast<double>(x0);
        const double fy = y - static_cast<double>(y0);
        const std::uint8_t* p = data_ + static_cast<std::size_t>(y0) * static_cast<std::size_t>(stride_) +
                                static_cast<std::size_t>(x0);
        const double top = static_cast<double>(p[0]) * (1.0 - fx) + static_cast<double>(p[1]) * fx;
        const double bot = static_cast<double>(p[stride_]) * (1.0 - fx) +
                           static_cast<double>(p[stride_ + 1]) * fx;
        return top * (1.0 - fy) + bot * fy;
    }

  private:
    const std::uint8_t* data_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    std::ptrdiff_t stride_ = 0;
};

// Owning image. Used by the simulator, the replay harness and tests. The live receiver
// never allocates one of these on the hot path -- it wraps the camera buffer in a view.
class Image8 {
  public:
    Image8() = default;
    Image8(int width, int height, std::uint8_t fill = 0)
        : w_(width), h_(height),
          px_(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill) {}

    [[nodiscard]] int width() const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }

    [[nodiscard]] std::uint8_t at(int x, int y) const noexcept {
        return px_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
                   static_cast<std::size_t>(x)];
    }
    void set(int x, int y, std::uint8_t v) noexcept {
        px_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
            static_cast<std::size_t>(x)] = v;
    }

    [[nodiscard]] ImageView8 view() const noexcept { return {px_.data(), w_, h_, w_}; }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return px_; }
    [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return px_; }

  private:
    int w_ = 0;
    int h_ = 0;
    std::vector<std::uint8_t> px_;
};

}  // namespace fileflow
