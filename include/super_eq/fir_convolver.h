#pragma once

#include "fft.h"
#include "types.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace super_eq {

class DelayLine {
public:
    explicit DelayLine(size_t max_samples = 65536) {
        resize(max_samples);
    }

    void resize(size_t max_samples) {
        size_t n = 1;
        size_t target = std::max<size_t>(max_samples + 4, 16);
        while (n < target) n <<= 1;
        buf_.assign(n, 0.0);
        mask_ = n - 1;
        write_idx_ = 0;
    }

    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0);
        write_idx_ = 0;
    }

    void set_delay_samples(double delay_samples) {
        delay_samples_ = std::max(0.0, delay_samples);
        size_t needed = static_cast<size_t>(std::ceil(delay_samples_)) + 4;
        if (needed >= buf_.size()) {
            resize(needed);
        }
    }

    double delay_samples() const { return delay_samples_; }

    inline double process(double x) {
        buf_[write_idx_] = x;
        double out = x;
        if (delay_samples_ > 0.0) {
            size_t int_d = static_cast<size_t>(delay_samples_);
            double frac = delay_samples_ - static_cast<double>(int_d);
            size_t idx0 = (write_idx_ - int_d) & mask_;
            if (frac < 1e-9) {
                out = buf_[idx0];
            } else {
                size_t idx1 = (write_idx_ - int_d - 1) & mask_;
                out = buf_[idx0] * (1.0 - frac) + buf_[idx1] * frac;
            }
        }
        write_idx_ = (write_idx_ + 1) & mask_;
        return out;
    }

private:
    std::vector<double> buf_;
    size_t mask_ = 0;
    size_t write_idx_ = 0;
    double delay_samples_ = 0.0;
};

class LinearPhaseFIRConvolver {
public:
    LinearPhaseFIRConvolver() = default;

    void configure(const std::function<double(double freq_hz)>& mag_linear_fn,
                   double sample_rate,
                   uint32_t fft_order = kDefaultFirFftOrder) {
        fft_order = std::max(kMinFirFftOrder, std::min(kMaxFirFftOrder, fft_order));
        const size_t design_n = size_t{1} << fft_order;
        num_taps_ = (design_n / 4) + 1;
        filter_latency_ = (num_taps_ - 1) / 2;
        hop_size_ = std::min<size_t>(512, design_n / 8);

        size_t conv_n = 1;
        while (conv_n < hop_size_ + num_taps_ - 1) {
            conv_n <<= 1;
        }
        convolve_fft_size_ = conv_n;
        fft_.resize(convolve_fft_size_);

        // 1. Design ideal linear-phase impulse response in frequency domain
        FFT design_fft(design_n);
        std::vector<std::complex<double>> H_design(design_n, std::complex<double>(0.0, 0.0));
        const double center = static_cast<double>(filter_latency_);
        const size_t half = design_n / 2;

        for (size_t k = 0; k <= half; ++k) {
            double freq = (static_cast<double>(k) * sample_rate) / static_cast<double>(design_n);
            double mag = std::max(0.0, mag_linear_fn(freq));
            double phase = -2.0 * kPi * static_cast<double>(k) * center / static_cast<double>(design_n);
            std::complex<double> val(mag * std::cos(phase), mag * std::sin(phase));
            if (k == 0 || k == half) {
                val = std::complex<double>(val.real(), 0.0);
            }
            H_design[k] = val;
            if (k > 0 && k < half) {
                H_design[design_n - k] = std::conj(val);
            }
        }
        design_fft.inverse(H_design);

        // 2. Window truncated impulse with Blackman-Harris window centered at filter_latency_
        std::vector<double> win;
        fill_blackman_harris(win, num_taps_);
        kernel_.resize(num_taps_);
        for (size_t i = 0; i < num_taps_; ++i) {
            kernel_[i] = H_design[i].real() * win[i];
        }

        // 3. Precompute kernel spectrum for runtime Overlap-Add
        H_freq_.assign(convolve_fft_size_, std::complex<double>(0.0, 0.0));
        for (size_t i = 0; i < num_taps_; ++i) {
            H_freq_[i] = std::complex<double>(kernel_[i], 0.0);
        }
        fft_.forward(H_freq_);

        // 4. Allocate/reset OLA state buffers only if geometry changed
        if (input_buf_.size() != hop_size_ || overlap_buf_.size() != convolve_fft_size_) {
            input_buf_.assign(hop_size_, 0.0);
            output_buf_.assign(hop_size_, 0.0);
            overlap_buf_.assign(convolve_fft_size_, 0.0);
            fft_buf_.assign(convolve_fft_size_, std::complex<double>(0.0, 0.0));
            pos_ = 0;
        }
        configured_ = true;
    }

    void reset() {
        std::fill(input_buf_.begin(), input_buf_.end(), 0.0);
        std::fill(output_buf_.begin(), output_buf_.end(), 0.0);
        std::fill(overlap_buf_.begin(), overlap_buf_.end(), 0.0);
        pos_ = 0;
    }

    bool is_configured() const { return configured_; }
    size_t num_taps() const { return num_taps_; }
    size_t hop_size() const { return hop_size_; }
    size_t filter_latency_samples() const { return filter_latency_; }
    size_t latency_samples() const { return configured_ ? (hop_size_ + filter_latency_) : 0; }
    const std::vector<double>& kernel() const { return kernel_; }

    inline double process_sample(double x) {
        if (!configured_ || hop_size_ == 0) return x;
        double out = output_buf_[pos_];
        input_buf_[pos_] = x;
        ++pos_;
        if (pos_ >= hop_size_) {
            convolve_block();
            pos_ = 0;
        }
        return out;
    }

private:
    void convolve_block() {
        for (size_t i = 0; i < hop_size_; ++i) {
            fft_buf_[i] = std::complex<double>(input_buf_[i], 0.0);
        }
        for (size_t i = hop_size_; i < convolve_fft_size_; ++i) {
            fft_buf_[i] = std::complex<double>(0.0, 0.0);
        }
        fft_.forward(fft_buf_);
        for (size_t i = 0; i < convolve_fft_size_; ++i) {
            fft_buf_[i] *= H_freq_[i];
        }
        fft_.inverse(fft_buf_);

        for (size_t i = 0; i < convolve_fft_size_; ++i) {
            overlap_buf_[i] += fft_buf_[i].real();
        }
        for (size_t i = 0; i < hop_size_; ++i) {
            output_buf_[i] = overlap_buf_[i];
        }
        const size_t tail = convolve_fft_size_ - hop_size_;
        for (size_t i = 0; i < tail; ++i) {
            overlap_buf_[i] = overlap_buf_[i + hop_size_];
        }
        for (size_t i = tail; i < convolve_fft_size_; ++i) {
            overlap_buf_[i] = 0.0;
        }
    }

    bool configured_ = false;
    size_t num_taps_ = 0;
    size_t filter_latency_ = 0;
    size_t hop_size_ = 0;
    size_t convolve_fft_size_ = 0;
    size_t pos_ = 0;

    FFT fft_;
    std::vector<double> kernel_;
    std::vector<std::complex<double>> H_freq_;
    std::vector<double> input_buf_;
    std::vector<double> output_buf_;
    std::vector<double> overlap_buf_;
    std::vector<std::complex<double>> fft_buf_;
};

} // namespace super_eq
