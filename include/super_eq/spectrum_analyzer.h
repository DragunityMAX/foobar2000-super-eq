#pragma once

#include "fft.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <vector>

namespace super_eq {

struct AudioAnalysisFrame {
    float pre_l = 0.0f;
    float pre_r = 0.0f;
    float post_l = 0.0f;
    float post_r = 0.0f;
};

class AudioRingBuffer {
public:
    static constexpr size_t kCapacity = 32768; // power of 2

    void push(const AudioAnalysisFrame& frame) {
        size_t w = write_idx_.load(std::memory_order_relaxed);
        size_t r = read_idx_.load(std::memory_order_acquire);
        if (w - r >= kCapacity) {
            // Drop oldest sample if consumer falls behind
            read_idx_.store(w - kCapacity + 1, std::memory_order_release);
        }
        buf_[w & (kCapacity - 1)] = frame;
        write_idx_.store(w + 1, std::memory_order_release);
    }

    size_t pop_batch(AudioAnalysisFrame* out, size_t max_frames) {
        size_t r = read_idx_.load(std::memory_order_relaxed);
        size_t w = write_idx_.load(std::memory_order_acquire);
        size_t avail = w - r;
        size_t to_read = std::min(avail, max_frames);
        for (size_t i = 0; i < to_read; ++i) {
            out[i] = buf_[(r + i) & (kCapacity - 1)];
        }
        read_idx_.store(r + to_read, std::memory_order_release);
        return to_read;
    }

    size_t available() const {
        size_t w = write_idx_.load(std::memory_order_acquire);
        size_t r = read_idx_.load(std::memory_order_acquire);
        return w - r;
    }

    void clear() {
        read_idx_.store(0, std::memory_order_relaxed);
        write_idx_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<AudioAnalysisFrame, kCapacity> buf_{};
    std::atomic<size_t> write_idx_{0};
    std::atomic<size_t> read_idx_{0};
};

class SpectrumAnalyzer {
public:
    static constexpr size_t kDefaultFftSize = 4096;
    static constexpr size_t kDefaultNumPoints = 256;
    static constexpr double kMinDisplayFreq = 20.0;
    static constexpr double kMaxDisplayFreq = 20000.0;
    static constexpr float kFloorDb = -120.0f;

    explicit SpectrumAnalyzer(size_t fft_size = kDefaultFftSize,
                              size_t num_points = kDefaultNumPoints) {
        configure(fft_size, num_points);
    }

    void configure(size_t fft_size, size_t num_points) {
        size_t n = 1;
        while (n < fft_size) n <<= 1;
        fft_size_ = std::max<size_t>(256, n);
        num_points_ = std::max<size_t>(32, num_points);

        fft_.resize(fft_size_);
        fill_blackman_harris(window_, fft_size_);
        double win_sum = 0.0;
        for (double w : window_) win_sum += w;
        win_norm_ = (win_sum > 0.0) ? (2.0 / win_sum) : 1.0;

        hist_pre_l_.assign(fft_size_, 0.0f);
        hist_pre_r_.assign(fft_size_, 0.0f);
        hist_post_l_.assign(fft_size_, 0.0f);
        hist_post_r_.assign(fft_size_, 0.0f);
        hist_pos_ = 0;
        samples_since_fft_ = 0;

        fft_buf_.resize(fft_size_);
        bin_mags_db_.resize(fft_size_ / 2 + 1, kFloorDb);

        pre_left_db_.assign(num_points_, kFloorDb);
        pre_right_db_.assign(num_points_, kFloorDb);
        post_left_db_.assign(num_points_, kFloorDb);
        post_right_db_.assign(num_points_, kFloorDb);
        peak_hold_db_.assign(num_points_, kFloorDb);

        freqs_.resize(num_points_);
        double log_min = std::log10(kMinDisplayFreq);
        double log_max = std::log10(kMaxDisplayFreq);
        for (size_t i = 0; i < num_points_; ++i) {
            double t = static_cast<double>(i) / static_cast<double>(num_points_ - 1);
            freqs_[i] = std::pow(10.0, log_min + t * (log_max - log_min));
        }
    }

    void set_smoothing(float smoothing) {
        smoothing_ = static_cast<float>(clamp_double(smoothing, 0.0, 0.96));
    }

    void reset() {
        std::fill(hist_pre_l_.begin(), hist_pre_l_.end(), 0.0f);
        std::fill(hist_pre_r_.begin(), hist_pre_r_.end(), 0.0f);
        std::fill(hist_post_l_.begin(), hist_post_l_.end(), 0.0f);
        std::fill(hist_post_r_.begin(), hist_post_r_.end(), 0.0f);
        std::fill(pre_left_db_.begin(), pre_left_db_.end(), kFloorDb);
        std::fill(pre_right_db_.begin(), pre_right_db_.end(), kFloorDb);
        std::fill(post_left_db_.begin(), post_left_db_.end(), kFloorDb);
        std::fill(post_right_db_.begin(), post_right_db_.end(), kFloorDb);
        std::fill(peak_hold_db_.begin(), peak_hold_db_.end(), kFloorDb);
        hist_pos_ = 0;
        samples_since_fft_ = 0;
    }

    void push_frames(const AudioAnalysisFrame* frames, size_t count, double sample_rate) {
        if (!frames || count == 0 || sample_rate <= 0.0) return;
        for (size_t i = 0; i < count; ++i) {
            hist_pre_l_[hist_pos_] = frames[i].pre_l;
            hist_pre_r_[hist_pos_] = frames[i].pre_r;
            hist_post_l_[hist_pos_] = frames[i].post_l;
            hist_post_r_[hist_pos_] = frames[i].post_r;
            hist_pos_ = (hist_pos_ + 1) % fft_size_;
            ++samples_since_fft_;
        }
        if (samples_since_fft_ >= fft_size_ / 4) {
            compute_all(sample_rate);
            samples_since_fft_ = 0;
        }
    }

    bool update_from_ring(AudioRingBuffer& ring, double sample_rate) {
        constexpr size_t kBatch = 2048;
        AudioAnalysisFrame tmp[kBatch];
        size_t total = 0;
        while (true) {
            size_t n = ring.pop_batch(tmp, kBatch);
            if (n == 0) break;
            push_frames(tmp, n, sample_rate);
            total += n;
        }
        if (total > 0) {
            compute_all(sample_rate);
            samples_since_fft_ = 0;
            return true;
        }
        // Gentle decay when idle
        for (size_t i = 0; i < num_points_; ++i) {
            peak_hold_db_[i] = std::max(kFloorDb, peak_hold_db_[i] - 0.5f);
        }
        return false;
    }

    size_t fft_size() const { return fft_size_; }
    size_t num_points() const { return num_points_; }
    const std::vector<double>& freqs() const { return freqs_; }
    const std::vector<float>& pre_left_db() const { return pre_left_db_; }
    const std::vector<float>& pre_right_db() const { return pre_right_db_; }
    const std::vector<float>& post_left_db() const { return post_left_db_; }
    const std::vector<float>& post_right_db() const { return post_right_db_; }
    const std::vector<float>& peak_hold_db() const { return peak_hold_db_; }

private:
    void compute_channel(const std::vector<float>& hist, double sample_rate, std::vector<float>& out_db) {
        for (size_t i = 0; i < fft_size_; ++i) {
            size_t idx = (hist_pos_ + i) % fft_size_;
            fft_buf_[i] = std::complex<double>(static_cast<double>(hist[idx]) * window_[i], 0.0);
        }
        fft_.forward(fft_buf_);

        const size_t half = fft_size_ / 2;
        for (size_t k = 0; k <= half; ++k) {
            double mag = std::abs(fft_buf_[k]) * win_norm_;
            if (k == 0 || k == half) mag *= 0.5;
            double db = (mag > 1e-6) ? 20.0 * std::log10(mag) : static_cast<double>(kFloorDb);
            bin_mags_db_[k] = static_cast<float>(std::max(static_cast<double>(kFloorDb), db));
        }

        const double bin_hz = sample_rate / static_cast<double>(fft_size_);
        for (size_t p = 0; p < num_points_; ++p) {
            double f_center = freqs_[p];
            double f_lo = (p == 0) ? f_center : 0.5 * (freqs_[p - 1] + f_center);
            double f_hi = (p + 1 == num_points_) ? f_center : 0.5 * (f_center + freqs_[p + 1]);

            double exact_bin = f_center / bin_hz;
            size_t b_lo = static_cast<size_t>(std::floor(f_lo / bin_hz));
            size_t b_hi = static_cast<size_t>(std::ceil(f_hi / bin_hz));
            b_lo = std::min(b_lo, half);
            b_hi = std::min(b_hi, half);

            float val_db = kFloorDb;
            if (b_hi <= b_lo + 1) {
                size_t b0 = std::min(static_cast<size_t>(std::floor(exact_bin)), half);
                size_t b1 = std::min(b0 + 1, half);
                double frac = exact_bin - static_cast<double>(b0);
                val_db = static_cast<float>(bin_mags_db_[b0] * (1.0 - frac) + bin_mags_db_[b1] * frac);
            } else {
                float max_db = kFloorDb;
                for (size_t b = b_lo; b <= b_hi; ++b) {
                    if (bin_mags_db_[b] > max_db) max_db = bin_mags_db_[b];
                }
                val_db = max_db;
            }
            out_db[p] = smoothing_ * out_db[p] + (1.0f - smoothing_) * val_db;
        }
    }

    void compute_all(double sample_rate) {
        compute_channel(hist_pre_l_, sample_rate, pre_left_db_);
        compute_channel(hist_pre_r_, sample_rate, pre_right_db_);
        compute_channel(hist_post_l_, sample_rate, post_left_db_);
        compute_channel(hist_post_r_, sample_rate, post_right_db_);
        for (size_t i = 0; i < num_points_; ++i) {
            float m = std::max(post_left_db_[i], post_right_db_[i]);
            peak_hold_db_[i] = std::max(m, peak_hold_db_[i] - 0.4f);
        }
    }

    size_t fft_size_ = kDefaultFftSize;
    size_t num_points_ = kDefaultNumPoints;
    float smoothing_ = 0.72f;
    double win_norm_ = 1.0;

    FFT fft_;
    std::vector<double> window_;
    std::vector<double> freqs_;
    std::vector<float> hist_pre_l_, hist_pre_r_, hist_post_l_, hist_post_r_;
    size_t hist_pos_ = 0;
    size_t samples_since_fft_ = 0;

    std::vector<std::complex<double>> fft_buf_;
    std::vector<float> bin_mags_db_;
    std::vector<float> pre_left_db_, pre_right_db_, post_left_db_, post_right_db_, peak_hold_db_;
};

} // namespace super_eq
