#pragma once

#include "biquad.h"
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace super_eq {

class FFT {
public:
    explicit FFT(size_t size = 0) {
        if (size > 0) {
            resize(size);
        }
    }

    void resize(size_t size) {
        // Ensure power of two
        size_t n = 1;
        while (n < size) n <<= 1;
        size_ = n;

        bitrev_.resize(size_);
        size_t log2n = 0;
        while ((size_t{1} << log2n) < size_) ++log2n;

        for (size_t i = 0; i < size_; ++i) {
            size_t rev = 0;
            for (size_t b = 0; b < log2n; ++b) {
                if ((i >> b) & 1u) {
                    rev |= (size_t{1} << (log2n - 1 - b));
                }
            }
            bitrev_[i] = rev;
        }

        twiddles_.resize(size_ / 2);
        for (size_t i = 0; i < size_ / 2; ++i) {
            double angle = -2.0 * kPi * static_cast<double>(i) / static_cast<double>(size_);
            twiddles_[i] = std::complex<double>(std::cos(angle), std::sin(angle));
        }
    }

    size_t size() const { return size_; }

    void forward(std::vector<std::complex<double>>& buf) const {
        transform(buf, false);
    }

    void inverse(std::vector<std::complex<double>>& buf) const {
        transform(buf, true);
        const double inv_n = 1.0 / static_cast<double>(size_);
        for (auto& v : buf) {
            v *= inv_n;
        }
    }

private:
    void transform(std::vector<std::complex<double>>& buf, bool inv) const {
        if (size_ == 0 || buf.size() < size_) return;

        for (size_t i = 0; i < size_; ++i) {
            size_t j = bitrev_[i];
            if (i < j) {
                std::swap(buf[i], buf[j]);
            }
        }

        for (size_t len = 2; len <= size_; len <<= 1) {
            size_t half = len >> 1;
            size_t step = size_ / len;
            for (size_t i = 0; i < size_; i += len) {
                for (size_t j = 0; j < half; ++j) {
                    std::complex<double> w = twiddles_[j * step];
                    if (inv) {
                        w = std::conj(w);
                    }
                    std::complex<double> u = buf[i + j];
                    std::complex<double> t = w * buf[i + j + half];
                    buf[i + j] = u + t;
                    buf[i + j + half] = u - t;
                }
            }
        }
    }

    size_t size_ = 0;
    std::vector<size_t> bitrev_;
    std::vector<std::complex<double>> twiddles_;
};

inline void fill_blackman_harris(std::vector<double>& win, size_t n) {
    win.resize(n);
    if (n == 0) return;
    if (n == 1) {
        win[0] = 1.0;
        return;
    }
    constexpr double a0 = 0.35875;
    constexpr double a1 = 0.48829;
    constexpr double a2 = 0.14128;
    constexpr double a3 = 0.01168;
    const double denom = static_cast<double>(n - 1);
    for (size_t i = 0; i < n; ++i) {
        double x = 2.0 * kPi * static_cast<double>(i) / denom;
        win[i] = a0 - a1 * std::cos(x) + a2 * std::cos(2.0 * x) - a3 * std::cos(3.0 * x);
    }
}

} // namespace super_eq
