#pragma once

#include "biquad.h"
#include "fir_convolver.h"
#include "spectrum_analyzer.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <mutex>
#include <vector>

namespace super_eq {

struct MeterSnapshot {
    float peak_l = 0.0f;
    float peak_r = 0.0f;
    float rms_l = 0.0f;
    float rms_r = 0.0f;
    bool clip_l = false;
    bool clip_r = false;
    uint32_t sample_rate = 44100;
    uint32_t channels = 2;
    size_t latency_samples = 0;
};

class SuperEQEngine {
public:
    static constexpr size_t kMaxSurroundExtraChannels = 14; // Supports up to 16 total channels

    SuperEQEngine() {
        state_ = EQPresetState::create_default_10band();
        rebuild_filters(44100.0);
    }

    void set_state(const EQPresetState& new_state) {
        EQPresetState clamped = new_state;
        clamped.clamp();
        std::lock_guard<std::mutex> lock(state_mutex_);
        pending_state_ = std::move(clamped);
        has_pending_.store(true, std::memory_order_release);
    }

    // Immediate synchronous apply (useful in offline/unit tests or constructor)
    void apply_state_now(const EQPresetState& new_state, double sample_rate) {
        EQPresetState clamped = new_state;
        clamped.clamp();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            state_ = clamped;
            has_pending_.store(false, std::memory_order_release);
        }
        rebuild_filters(sample_rate);
    }

    EQPresetState get_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return has_pending_.load(std::memory_order_acquire) ? pending_state_ : state_;
    }

    bool should_upmix_mono_to_stereo() const {
        EQPresetState st = get_state();
        return !st.global_bypass && st.has_lr_difference();
    }

    void reset() {
        for (auto& c : left_chain_) c.reset();
        for (auto& c : right_chain_) c.reset();
        for (auto& c : mid_chain_) c.reset();
        for (auto& c : side_chain_) c.reset();
        for (auto& c : solo_left_chain_) c.reset();
        for (auto& c : solo_right_chain_) c.reset();
        for (auto& c : solo_mid_chain_) c.reset();
        for (auto& c : solo_side_chain_) c.reset();
        left_fir_.reset();
        right_fir_.reset();
        mid_fir_.reset();
        side_fir_.reset();
        left_delay_.reset();
        right_delay_.reset();
        for (auto& d : extra_latency_delays_) d.reset();
    }

    void reset_clip_latches() {
        std::lock_guard<std::mutex> lock(meter_mutex_);
        meters_.clip_l = false;
        meters_.clip_r = false;
    }

    MeterSnapshot get_meters() const {
        std::lock_guard<std::mutex> lock(meter_mutex_);
        return meters_;
    }

    AudioRingBuffer& ring_buffer() { return ring_buffer_; }
    const AudioRingBuffer& ring_buffer() const { return ring_buffer_; }

    size_t latency_samples() const {
        return current_latency_samples_.load(std::memory_order_acquire);
    }

    double sample_rate() const { return sample_rate_; }

    void process_interleaved(float* samples, size_t frames, unsigned channels, unsigned sample_rate) {
        if (!samples || frames == 0 || channels == 0 || sample_rate == 0) return;

        bool need_rebuild = false;
        if (std::abs(static_cast<double>(sample_rate) - sample_rate_) > 0.5) {
            need_rebuild = true;
        }
        if (has_pending_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (has_pending_.load(std::memory_order_relaxed)) {
                state_ = pending_state_;
                has_pending_.store(false, std::memory_order_release);
                need_rebuild = true;
            }
        }
        if (need_rebuild) {
            rebuild_filters(static_cast<double>(sample_rate));
        }

        const bool bypass = state_.global_bypass;
        const bool fir_mode = (!bypass && state_.processing_mode == ProcessingMode::LinearPhaseFIR);
        const bool solo_mode = (!bypass && state_.has_solo_bands());
        const bool ms_active = (!bypass && state_.has_ms_bands());
        const size_t lat = current_latency_samples_.load(std::memory_order_acquire);

        const double global_lin = db_to_linear(state_.global_preamp_db) * auto_gain_linear_;
        const double l_gain = state_.left_cal.mute ? 0.0
                              : global_lin * db_to_linear(state_.left_cal.preamp_db) *
                                    (state_.left_cal.invert_phase ? -1.0 : 1.0);
        const double r_gain = state_.right_cal.mute ? 0.0
                              : global_lin * db_to_linear(state_.right_cal.preamp_db) *
                                    (state_.right_cal.invert_phase ? -1.0 : 1.0);

        float block_peak_l = 0.0f, block_peak_r = 0.0f;
        double sum_sq_l = 0.0, sum_sq_r = 0.0;
        bool block_clip_l = false, block_clip_r = false;

        for (size_t i = 0; i < frames; ++i) {
            float* frame = samples + i * channels;
            double pre_l = static_cast<double>(frame[0]);
            double pre_r = (channels >= 2) ? static_cast<double>(frame[1]) : pre_l;

            double l = pre_l;
            double r = pre_r;

            if (!bypass) {
                if (solo_mode) {
                    double sl = 0.0;
                    for (auto& c : solo_left_chain_) sl += c.process(pre_l);
                    double sr = 0.0;
                    if (channels >= 2) {
                        for (auto& c : solo_right_chain_) sr += c.process(pre_r);
                    }
                    double sm = 0.0;
                    if (!solo_mid_chain_.empty()) {
                        double m = 0.5 * (pre_l + pre_r);
                        for (auto& c : solo_mid_chain_) sm += c.process(m);
                    }
                    double ss = 0.0;
                    if (!solo_side_chain_.empty() && channels >= 2) {
                        double s = 0.5 * (pre_l - pre_r);
                        for (auto& c : solo_side_chain_) ss += c.process(s);
                    }
                    l = sl + sm + ss;
                    r = sr + sm - ss;
                } else if (fir_mode) {
                    l = left_fir_.process_sample(l);
                    if (channels >= 2) {
                        r = right_fir_.process_sample(r);
                    }
                    if (ms_active && channels >= 2) {
                        double m = 0.5 * (l + r);
                        double s = 0.5 * (l - r);
                        m = mid_fir_.process_sample(m);
                        s = side_fir_.process_sample(s);
                        l = m + s;
                        r = m - s;
                    }
                } else {
                    for (auto& sec : left_chain_) {
                        l = sec.process(l);
                    }
                    if (channels >= 2) {
                        for (auto& sec : right_chain_) {
                            r = sec.process(r);
                        }
                        if (ms_active) {
                            double m = 0.5 * (l + r);
                            double s = 0.5 * (l - r);
                            for (auto& sec : mid_chain_) m = sec.process(m);
                            for (auto& sec : side_chain_) s = sec.process(s);
                            l = m + s;
                            r = m - s;
                        }
                    }
                }

                // Per-channel calibration: preamp, phase polarity, delay, clip limiter
                l *= l_gain;
                l = left_delay_.process(l);
                if (state_.clip_limiter) {
                    l = soft_limit(l);
                }

                if (channels >= 2) {
                    r *= r_gain;
                    r = right_delay_.process(r);
                    if (state_.clip_limiter) {
                        r = soft_limit(r);
                    }
                }

                // Surround channels (C, LFE, BL, BR, SL, SR, etc.) latency alignment in Linear-Phase FIR mode
                if (channels > 2 && lat > 0) {
                    const size_t extra = std::min<size_t>(channels - 2, kMaxSurroundExtraChannels);
                    for (size_t ch = 0; ch < extra; ++ch) {
                        double x = static_cast<double>(frame[2 + ch]);
                        frame[2 + ch] = static_cast<float>(extra_latency_delays_[ch].process(x));
                    }
                }
            }

            frame[0] = static_cast<float>(l);
            if (channels >= 2) {
                frame[1] = static_cast<float>(r);
            } else {
                r = l;
            }

            float fl = static_cast<float>(l);
            float fr = static_cast<float>(r);
            float al = std::abs(fl);
            float ar = std::abs(fr);
            if (al > block_peak_l) block_peak_l = al;
            if (ar > block_peak_r) block_peak_r = ar;
            if (al >= 0.9999f) block_clip_l = true;
            if (ar >= 0.9999f) block_clip_r = true;
            sum_sq_l += l * l;
            sum_sq_r += r * r;

            AudioAnalysisFrame af;
            af.pre_l = static_cast<float>(pre_l);
            af.pre_r = static_cast<float>(pre_r);
            af.post_l = fl;
            af.post_r = fr;
            ring_buffer_.push(af);
        }

        float rms_l = static_cast<float>(std::sqrt(sum_sq_l / static_cast<double>(frames)));
        float rms_r = static_cast<float>(std::sqrt(sum_sq_r / static_cast<double>(frames)));
        {
            std::lock_guard<std::mutex> lock(meter_mutex_);
            meters_.peak_l = std::max(block_peak_l, meters_.peak_l * 0.85f);
            meters_.peak_r = std::max(block_peak_r, meters_.peak_r * 0.85f);
            meters_.rms_l = rms_l;
            meters_.rms_r = rms_r;
            if (block_clip_l) meters_.clip_l = true;
            if (block_clip_r) meters_.clip_r = true;
            meters_.sample_rate = sample_rate;
            meters_.channels = channels;
            meters_.latency_samples = lat;
        }
    }

    // UI Response Curve Evaluation Helpers
    static std::complex<double> eval_chain_transfer(const std::vector<BiquadCascade>& chain,
                                                    double freq_hz,
                                                    double sample_rate) {
        std::complex<double> h(1.0, 0.0);
        for (const auto& sec : chain) {
            h *= sec.eval_transfer(freq_hz, sample_rate);
        }
        return h;
    }

    static std::complex<double> eval_route_transfer(const EQPresetState& st,
                                                    ChannelRoute route,
                                                    double freq_hz,
                                                    double sample_rate) {
        std::complex<double> h(1.0, 0.0);
        for (const auto& b : st.bands) {
            if (!b.enabled || b.route != route) continue;
            h *= BiquadCascade::design(b, sample_rate).eval_transfer(freq_hz, sample_rate);
        }
        return h;
    }

    static std::complex<double> eval_channel_transfer(const EQPresetState& st,
                                                      ChannelRoute view_target,
                                                      double freq_hz,
                                                      double sample_rate,
                                                      bool include_preamp = false) {
        if (st.global_bypass) {
            return std::complex<double>(1.0, 0.0);
        }
        std::complex<double> h_stereo = eval_route_transfer(st, ChannelRoute::Stereo, freq_hz, sample_rate);
        std::complex<double> h_mid = eval_route_transfer(st, ChannelRoute::Mid, freq_hz, sample_rate);
        std::complex<double> h_side = eval_route_transfer(st, ChannelRoute::Side, freq_hz, sample_rate);
        std::complex<double> h_ms_eff = 0.5 * (h_mid + h_side);

        std::complex<double> h(1.0, 0.0);
        double cal_gain = 1.0;

        switch (view_target) {
            case ChannelRoute::Left:
                h = h_stereo * eval_route_transfer(st, ChannelRoute::Left, freq_hz, sample_rate) * h_ms_eff;
                if (include_preamp) {
                    cal_gain = db_to_linear(st.global_preamp_db + st.left_cal.preamp_db) *
                               (st.left_cal.invert_phase ? -1.0 : 1.0);
                }
                break;
            case ChannelRoute::Right:
                h = h_stereo * eval_route_transfer(st, ChannelRoute::Right, freq_hz, sample_rate) * h_ms_eff;
                if (include_preamp) {
                    cal_gain = db_to_linear(st.global_preamp_db + st.right_cal.preamp_db) *
                               (st.right_cal.invert_phase ? -1.0 : 1.0);
                }
                break;
            case ChannelRoute::Mid:
                h = h_stereo * h_mid;
                break;
            case ChannelRoute::Side:
                h = h_stereo * h_side;
                break;
            case ChannelRoute::Stereo:
                h = h_stereo;
                break;
        }
        return h * cal_gain;
    }

    static double eval_channel_magnitude_db(const EQPresetState& st,
                                            ChannelRoute view_target,
                                            double freq_hz,
                                            double sample_rate,
                                            bool include_preamp = false) {
        return linear_to_db(std::abs(eval_channel_transfer(st, view_target, freq_hz, sample_rate, include_preamp)));
    }

    static double eval_channel_phase_deg(const EQPresetState& st,
                                         ChannelRoute view_target,
                                         double freq_hz,
                                         double sample_rate) {
        if (st.processing_mode == ProcessingMode::LinearPhaseFIR) {
            return 0.0;
        }
        std::complex<double> h = eval_channel_transfer(st, view_target, freq_hz, sample_rate, false);
        return std::arg(h) * (180.0 / kPi);
    }

    static double eval_single_band_magnitude_db(const BandParams& band,
                                                double freq_hz,
                                                double sample_rate) {
        if (!band.enabled) return 0.0;
        return BiquadCascade::design(band, sample_rate).magnitude_db(freq_hz, sample_rate);
    }

    static double compute_auto_gain_db(const EQPresetState& st, double sample_rate) {
        if (!st.auto_gain || st.global_bypass) return 0.0;
        constexpr int kSteps = 128;
        const double log_min = std::log10(20.0);
        const double log_max = std::log10(std::min(20000.0, 0.48 * sample_rate));
        double max_db = 0.0;
        for (int i = 0; i < kSteps; ++i) {
            double f = std::pow(10.0, log_min + (log_max - log_min) * i / (kSteps - 1));
            double ml = eval_channel_magnitude_db(st, ChannelRoute::Left, f, sample_rate, false);
            double mr = eval_channel_magnitude_db(st, ChannelRoute::Right, f, sample_rate, false);
            max_db = std::max(max_db, std::max(ml, mr));
        }
        return -max_db;
    }

private:
    static inline double soft_limit(double x) {
        constexpr double kThresh = 0.95;
        constexpr double kHeadroom = 0.049;
        double ax = std::abs(x);
        if (ax <= kThresh) return x;
        double limited = kThresh + kHeadroom * std::tanh((ax - kThresh) / kHeadroom);
        return (x >= 0.0) ? limited : -limited;
    }

    void rebuild_filters(double sample_rate) {
        sample_rate_ = (sample_rate > 0.0) ? sample_rate : 44100.0;

        left_chain_.clear();
        right_chain_.clear();
        mid_chain_.clear();
        side_chain_.clear();
        solo_left_chain_.clear();
        solo_right_chain_.clear();
        solo_mid_chain_.clear();
        solo_side_chain_.clear();

        for (const auto& b : state_.bands) {
            if (!b.enabled) continue;
            BiquadCascade cas = BiquadCascade::design(b, sample_rate_);
            if (cas.num_sections > 0) {
                switch (b.route) {
                    case ChannelRoute::Stereo:
                        left_chain_.push_back(cas);
                        right_chain_.push_back(cas);
                        break;
                    case ChannelRoute::Left:
                        left_chain_.push_back(cas);
                        break;
                    case ChannelRoute::Right:
                        right_chain_.push_back(cas);
                        break;
                    case ChannelRoute::Mid:
                        mid_chain_.push_back(cas);
                        break;
                    case ChannelRoute::Side:
                        side_chain_.push_back(cas);
                        break;
                }
            }
            if (b.solo) {
                BiquadCascade sc = BiquadCascade::design_solo_bandpass(b, sample_rate_);
                switch (b.route) {
                    case ChannelRoute::Stereo:
                        solo_left_chain_.push_back(sc);
                        solo_right_chain_.push_back(sc);
                        break;
                    case ChannelRoute::Left:
                        solo_left_chain_.push_back(sc);
                        break;
                    case ChannelRoute::Right:
                        solo_right_chain_.push_back(sc);
                        break;
                    case ChannelRoute::Mid:
                        solo_mid_chain_.push_back(sc);
                        break;
                    case ChannelRoute::Side:
                        solo_side_chain_.push_back(sc);
                        break;
                }
            }
        }

        // Calibration delays
        left_delay_.set_delay_samples((state_.left_cal.delay_ms * 0.001) * sample_rate_);
        right_delay_.set_delay_samples((state_.right_cal.delay_ms * 0.001) * sample_rate_);

        // Auto-gain
        auto_gain_linear_ = db_to_linear(compute_auto_gain_db(state_, sample_rate_));

        // Linear-Phase FIR configuration
        size_t total_lat = 0;
        if (!state_.global_bypass && state_.processing_mode == ProcessingMode::LinearPhaseFIR) {
            auto mag_left = [&](double f) {
                return std::abs(eval_chain_transfer(left_chain_, f, sample_rate_));
            };
            auto mag_right = [&](double f) {
                return std::abs(eval_chain_transfer(right_chain_, f, sample_rate_));
            };
            left_fir_.configure(mag_left, sample_rate_, state_.fir_fft_order);
            right_fir_.configure(mag_right, sample_rate_, state_.fir_fft_order);
            total_lat = left_fir_.latency_samples();

            if (state_.has_ms_bands()) {
                auto mag_mid = [&](double f) {
                    return std::abs(eval_chain_transfer(mid_chain_, f, sample_rate_));
                };
                auto mag_side = [&](double f) {
                    return std::abs(eval_chain_transfer(side_chain_, f, sample_rate_));
                };
                mid_fir_.configure(mag_mid, sample_rate_, state_.fir_fft_order);
                side_fir_.configure(mag_side, sample_rate_, state_.fir_fft_order);
                total_lat += mid_fir_.latency_samples();
            }
        }
        current_latency_samples_.store(total_lat, std::memory_order_release);

        for (auto& d : extra_latency_delays_) {
            d.set_delay_samples(static_cast<double>(total_lat));
        }
    }

    mutable std::mutex state_mutex_;
    EQPresetState state_;
    EQPresetState pending_state_;
    std::atomic<bool> has_pending_{false};

    double sample_rate_ = 44100.0;
    double auto_gain_linear_ = 1.0;
    std::atomic<size_t> current_latency_samples_{0};

    std::vector<BiquadCascade> left_chain_;
    std::vector<BiquadCascade> right_chain_;
    std::vector<BiquadCascade> mid_chain_;
    std::vector<BiquadCascade> side_chain_;

    std::vector<BiquadCascade> solo_left_chain_;
    std::vector<BiquadCascade> solo_right_chain_;
    std::vector<BiquadCascade> solo_mid_chain_;
    std::vector<BiquadCascade> solo_side_chain_;

    LinearPhaseFIRConvolver left_fir_;
    LinearPhaseFIRConvolver right_fir_;
    LinearPhaseFIRConvolver mid_fir_;
    LinearPhaseFIRConvolver side_fir_;

    DelayLine left_delay_;
    DelayLine right_delay_;
    std::array<DelayLine, kMaxSurroundExtraChannels> extra_latency_delays_;

    AudioRingBuffer ring_buffer_;
    mutable std::mutex meter_mutex_;
    MeterSnapshot meters_{};
};

} // namespace super_eq
