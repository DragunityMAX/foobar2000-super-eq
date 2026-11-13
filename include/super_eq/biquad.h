#pragma once

#include "types.h"
#include <array>
#include <cmath>
#include <complex>

namespace super_eq {

constexpr double kPi = 3.1415926535897932384626433832795;

struct BiquadCoeffs {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct BiquadSection {
    BiquadCoeffs c{};
    double z1 = 0.0;
    double z2 = 0.0;

    void reset() {
        z1 = 0.0;
        z2 = 0.0;
    }

    inline double process(double x) {
        double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        // Flush denormals
        if (std::abs(z1) < 1e-30) z1 = 0.0;
        if (std::abs(z2) < 1e-30) z2 = 0.0;
        return y;
    }

    std::complex<double> eval_transfer(double omega_rad) const {
        std::complex<double> e1 = std::polar(1.0, -omega_rad);
        std::complex<double> e2 = e1 * e1;
        std::complex<double> num = c.b0 + c.b1 * e1 + c.b2 * e2;
        std::complex<double> den = 1.0 + c.a1 * e1 + c.a2 * e2;
        return num / den;
    }
};

struct BiquadCascade {
    static constexpr size_t kMaxSections = 6;
    std::array<BiquadSection, kMaxSections> sections{};
    size_t num_sections = 0;

    void reset() {
        for (size_t i = 0; i < num_sections; ++i) {
            sections[i].reset();
        }
    }

    inline double process(double x) {
        double y = x;
        for (size_t i = 0; i < num_sections; ++i) {
            y = sections[i].process(y);
        }
        return y;
    }

    std::complex<double> eval_transfer(double freq_hz, double sample_rate) const {
        if (num_sections == 0 || sample_rate <= 0.0) {
            return std::complex<double>(1.0, 0.0);
        }
        double omega = 2.0 * kPi * freq_hz / sample_rate;
        std::complex<double> h(1.0, 0.0);
        for (size_t i = 0; i < num_sections; ++i) {
            h *= sections[i].eval_transfer(omega);
        }
        return h;
    }

    double magnitude_db(double freq_hz, double sample_rate) const {
        double mag = std::abs(eval_transfer(freq_hz, sample_rate));
        return linear_to_db(mag);
    }

    double phase_deg(double freq_hz, double sample_rate) const {
        std::complex<double> h = eval_transfer(freq_hz, sample_rate);
        return std::arg(h) * (180.0 / kPi);
    }

    static BiquadCoeffs make_first_order_lp(double fc, double fs) {
        double K = std::tan(kPi * fc / fs);
        double norm = 1.0 / (1.0 + K);
        BiquadCoeffs c;
        c.b0 = K * norm;
        c.b1 = c.b0;
        c.b2 = 0.0;
        c.a1 = (K - 1.0) * norm;
        c.a2 = 0.0;
        return c;
    }

    static BiquadCoeffs make_first_order_hp(double fc, double fs) {
        double K = std::tan(kPi * fc / fs);
        double norm = 1.0 / (1.0 + K);
        BiquadCoeffs c;
        c.b0 = norm;
        c.b1 = -norm;
        c.b2 = 0.0;
        c.a1 = (K - 1.0) * norm;
        c.a2 = 0.0;
        return c;
    }

    static BiquadCoeffs make_first_order_low_shelf(double fc, double fs, double gain_db) {
        double A = std::pow(10.0, gain_db / 40.0);
        double K = std::tan(kPi * fc / fs);
        // Bilinear 1st-order symmetric low shelf: H(s) = (s/wc + A) / (s/wc + 1/A)
        double b0_s = A;
        double a0_s = 1.0 / A;
        double norm = 1.0 / (1.0 + K * a0_s);
        BiquadCoeffs c;
        c.b0 = (1.0 + K * b0_s) * norm;
        c.b1 = (-1.0 + K * b0_s) * norm;
        c.b2 = 0.0;
        c.a1 = (-1.0 + K * a0_s) * norm;
        c.a2 = 0.0;
        return c;
    }

    static BiquadCoeffs make_first_order_high_shelf(double fc, double fs, double gain_db) {
        double A = std::pow(10.0, gain_db / 40.0);
        double K = std::tan(kPi * fc / fs);
        // H(s) = (A * s + wc) / (s / A + wc)
        double denom = (1.0 / A) + K;
        double norm = 1.0 / denom;
        BiquadCoeffs c;
        c.b0 = (A + K) * norm;
        c.b1 = (-A + K) * norm;
        c.b2 = 0.0;
        c.a1 = (-(1.0 / A) + K) * norm;
        c.a2 = 0.0;
        return c;
    }

    static BiquadCoeffs make_biquad(FilterType type, double fc, double fs, double gain_db, double q) {
        double w0 = 2.0 * kPi * fc / fs;
        double cos_w0 = std::cos(w0);
        double sin_w0 = std::sin(w0);
        double alpha = sin_w0 / (2.0 * q);
        double A = std::pow(10.0, gain_db / 40.0);

        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a0 = 1.0, a1 = 0.0, a2 = 0.0;

        switch (type) {
            case FilterType::Bell: {
                b0 = 1.0 + alpha * A;
                b1 = -2.0 * cos_w0;
                b2 = 1.0 - alpha * A;
                a0 = 1.0 + alpha / A;
                a1 = -2.0 * cos_w0;
                a2 = 1.0 - alpha / A;
                break;
            }
            case FilterType::LowShelf: {
                double sqrtA2alpha = 2.0 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + sqrtA2alpha);
                b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
                b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - sqrtA2alpha);
                a0 = (A + 1.0) + (A - 1.0) * cos_w0 + sqrtA2alpha;
                a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
                a2 = (A + 1.0) + (A - 1.0) * cos_w0 - sqrtA2alpha;
                break;
            }
            case FilterType::HighShelf: {
                double sqrtA2alpha = 2.0 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + sqrtA2alpha);
                b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
                b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - sqrtA2alpha);
                a0 = (A + 1.0) - (A - 1.0) * cos_w0 + sqrtA2alpha;
                a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
                a2 = (A + 1.0) - (A - 1.0) * cos_w0 - sqrtA2alpha;
                break;
            }
            case FilterType::LowPass: {
                b0 = (1.0 - cos_w0) * 0.5;
                b1 = 1.0 - cos_w0;
                b2 = (1.0 - cos_w0) * 0.5;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cos_w0;
                a2 = 1.0 - alpha;
                break;
            }
            case FilterType::HighPass: {
                b0 = (1.0 + cos_w0) * 0.5;
                b1 = -(1.0 + cos_w0);
                b2 = (1.0 + cos_w0) * 0.5;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cos_w0;
                a2 = 1.0 - alpha;
                break;
            }
            case FilterType::Notch: {
                b0 = 1.0;
                b1 = -2.0 * cos_w0;
                b2 = 1.0;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cos_w0;
                a2 = 1.0 - alpha;
                break;
            }
            case FilterType::BandPass: {
                // Constant 0 dB peak gain bandpass
                b0 = alpha;
                b1 = 0.0;
                b2 = -alpha;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cos_w0;
                a2 = 1.0 - alpha;
                break;
            }
            case FilterType::AllPass: {
                b0 = 1.0 - alpha;
                b1 = -2.0 * cos_w0;
                b2 = 1.0 + alpha;
                a0 = 1.0 + alpha;
                a1 = -2.0 * cos_w0;
                a2 = 1.0 - alpha;
                break;
            }
            case FilterType::FlatTilt:
                break;
        }

        BiquadCoeffs c;
        double inv_a0 = 1.0 / a0;
        c.b0 = b0 * inv_a0;
        c.b1 = b1 * inv_a0;
        c.b2 = b2 * inv_a0;
        c.a1 = a1 * inv_a0;
        c.a2 = a2 * inv_a0;
        return c;
    }

    static BiquadCascade design(const BandParams& raw_band, double sample_rate) {
        BiquadCascade cascade;
        if (!raw_band.enabled || sample_rate <= 0.0) {
            return cascade;
        }
        BandParams band = raw_band;
        band.clamp();

        double max_fc = 0.492 * sample_rate;
        double min_fc = 5.0;
        double fc = clamp_double(band.freq_hz, min_fc, max_fc);
        double q = clamp_double(band.q, kMinQ, kMaxQ);
        double gain = band.gain_db;

        auto push = [&](const BiquadCoeffs& c) {
            if (cascade.num_sections < kMaxSections) {
                cascade.sections[cascade.num_sections++].c = c;
            }
        };

        switch (band.type) {
            case FilterType::Bell: {
                if (std::abs(gain) < 1e-6) return cascade;
                push(make_biquad(FilterType::Bell, fc, sample_rate, gain, q));
                break;
            }
            case FilterType::LowShelf:
            case FilterType::HighShelf: {
                if (std::abs(gain) < 1e-6) return cascade;
                int poles = static_cast<int>(band.slope);
                if (poles <= 1) {
                    push(band.type == FilterType::LowShelf
                             ? make_first_order_low_shelf(fc, sample_rate, gain)
                             : make_first_order_high_shelf(fc, sample_rate, gain));
                } else {
                    int num_biquads = std::max(1, poles / 2);
                    double split_gain = gain / static_cast<double>(num_biquads);
                    for (int i = 0; i < num_biquads; ++i) {
                        push(make_biquad(band.type, fc, sample_rate, split_gain, q));
                    }
                }
                break;
            }
            case FilterType::LowPass:
            case FilterType::HighPass: {
                int poles = static_cast<int>(band.slope);
                if (poles == 1) {
                    push(band.type == FilterType::LowPass
                             ? make_first_order_lp(fc, sample_rate)
                             : make_first_order_hp(fc, sample_rate));
                } else {
                    bool has_odd = (poles % 2) == 1;
                    int num_pairs = poles / 2;
                    double q_scale = q / 0.7071067811865475;
                    for (int k = 0; k < num_pairs; ++k) {
                        double angle = kPi * (2.0 * k + 1.0 + (has_odd ? 1.0 : 0.0)) / (2.0 * poles);
                        // Actually for N-pole Butterworth: poles at angles pi*(2k + 1 + N)/(2N)
                        // Pair k (0..num_pairs-1) has Q = 1 / (2 * cos(pi*(2k+1)/(2N))) for even N,
                        // or sin(pi*(k+1)/N) in general:
                        (void)angle;
                        double pole_angle = kPi * (2.0 * k + 1.0) / (2.0 * poles);
                        double butter_q = 1.0 / (2.0 * std::sin(pole_angle));
                        // Apply user Q scaling to the highest-Q pair (k == 0)
                        double section_q = (k == 0) ? clamp_double(butter_q * q_scale, 0.1, 40.0) : butter_q;
                        push(make_biquad(band.type, fc, sample_rate, 0.0, section_q));
                    }
                    if (has_odd) {
                        push(band.type == FilterType::LowPass
                                 ? make_first_order_lp(fc, sample_rate)
                                 : make_first_order_hp(fc, sample_rate));
                    }
                }
                break;
            }
            case FilterType::Notch:
            case FilterType::BandPass:
            case FilterType::AllPass: {
                int poles = static_cast<int>(band.slope);
                int count = std::max(1, poles / 2);
                for (int i = 0; i < count; ++i) {
                    push(make_biquad(band.type, fc, sample_rate, 0.0, q));
                }
                break;
            }
            case FilterType::FlatTilt: {
                if (std::abs(gain) < 1e-6) return cascade;
                // Symmetrical LowShelf/HighShelf pair across 3 decades centered at fc
                // yields linear-in-log-frequency tilt with 0.0 dB at fc.
                double f_low = clamp_double(fc / 4.0, min_fc, max_fc);
                double f_high = clamp_double(fc * 4.0, min_fc, max_fc);
                double f_low2 = clamp_double(fc / 16.0, min_fc, max_fc);
                double f_high2 = clamp_double(fc * 16.0, min_fc, max_fc);
                // Split into two symmetric pairs for broad flat slope
                double half_g = gain * 0.5;
                push(make_biquad(FilterType::LowShelf, f_low, sample_rate, -half_g * 0.5, 0.45));
                push(make_biquad(FilterType::HighShelf, f_high, sample_rate, half_g * 0.5, 0.45));
                push(make_biquad(FilterType::LowShelf, f_low2, sample_rate, -half_g * 0.5, 0.45));
                push(make_biquad(FilterType::HighShelf, f_high2, sample_rate, half_g * 0.5, 0.45));
                break;
            }
        }
        return cascade;
    }

    static BiquadCascade design_solo_bandpass(const BandParams& raw_band, double sample_rate) {
        BiquadCascade cascade;
        if (sample_rate <= 0.0) return cascade;
        double fc = clamp_double(raw_band.freq_hz, 20.0, 0.48 * sample_rate);
        double q = clamp_double(raw_band.q, 0.5, 16.0);
        cascade.sections[0].c = make_biquad(FilterType::BandPass, fc, sample_rate, 0.0, q);
        cascade.num_sections = 1;
        return cascade;
    }
};

} // namespace super_eq
