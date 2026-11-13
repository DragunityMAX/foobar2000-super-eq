#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace super_eq {

constexpr size_t kMaxBands = 64;
constexpr double kMinFreqHz = 10.0;
constexpr double kMaxFreqHz = 30000.0;
constexpr double kMinGainDb = -30.0;
constexpr double kMaxGainDb = 30.0;
constexpr double kMinQ = 0.05;
constexpr double kMaxQ = 40.0;
constexpr double kMinDelayMs = 0.0;
constexpr double kMaxDelayMs = 100.0;
constexpr uint32_t kMinFirFftOrder = 11; // 2048
constexpr uint32_t kMaxFirFftOrder = 16; // 65536
constexpr uint32_t kDefaultFirFftOrder = 14; // 16384

enum class FilterType : uint8_t {
    Bell = 0,
    LowShelf = 1,
    HighShelf = 2,
    LowPass = 3,
    HighPass = 4,
    Notch = 5,
    BandPass = 6,
    AllPass = 7,
    FlatTilt = 8
};

enum class ChannelRoute : uint8_t {
    Stereo = 0, // L+R
    Left = 1,   // L only
    Right = 2,  // R only
    Mid = 3,    // M only
    Side = 4    // S only
};

enum class FilterSlope : uint8_t {
    dB6 = 1,
    dB12 = 2,
    dB18 = 3,
    dB24 = 4,
    dB36 = 6,
    dB48 = 8
};

enum class ProcessingMode : uint8_t {
    MinimumPhaseIIR = 0,
    LinearPhaseFIR = 1
};

inline double clamp_double(double v, double lo, double hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

inline double db_to_linear(double db) {
    return std::pow(10.0, db / 20.0);
}

inline double linear_to_db(double lin) {
    constexpr double kFloor = 1e-12;
    return 20.0 * std::log10((lin > kFloor) ? lin : kFloor);
}

struct BandParams {
    bool enabled = true;
    bool solo = false;
    FilterType type = FilterType::Bell;
    ChannelRoute route = ChannelRoute::Stereo;
    FilterSlope slope = FilterSlope::dB12;
    double freq_hz = 1000.0;
    double gain_db = 0.0;
    double q = 0.7071067811865475;

    void clamp() {
        freq_hz = clamp_double(freq_hz, kMinFreqHz, kMaxFreqHz);
        gain_db = clamp_double(gain_db, kMinGainDb, kMaxGainDb);
        q = clamp_double(q, kMinQ, kMaxQ);
    }

    bool operator==(const BandParams& o) const {
        return enabled == o.enabled &&
               solo == o.solo &&
               type == o.type &&
               route == o.route &&
               slope == o.slope &&
               freq_hz == o.freq_hz &&
               gain_db == o.gain_db &&
               q == o.q;
    }

    bool operator!=(const BandParams& o) const {
        return !(*this == o);
    }
};

struct ChannelCalibration {
    double preamp_db = 0.0;
    double delay_ms = 0.0;
    bool invert_phase = false;
    bool mute = false;

    void clamp() {
        preamp_db = clamp_double(preamp_db, kMinGainDb, kMaxGainDb);
        delay_ms = clamp_double(delay_ms, kMinDelayMs, kMaxDelayMs);
    }

    bool operator==(const ChannelCalibration& o) const {
        return preamp_db == o.preamp_db &&
               delay_ms == o.delay_ms &&
               invert_phase == o.invert_phase &&
               mute == o.mute;
    }

    bool operator!=(const ChannelCalibration& o) const {
        return !(*this == o);
    }
};

struct EQPresetState {
    std::string name = "Default Flat";
    bool global_bypass = false;
    bool link_lr = false;
    ProcessingMode processing_mode = ProcessingMode::MinimumPhaseIIR;
    uint32_t fir_fft_order = kDefaultFirFftOrder;
    bool auto_gain = false;
    bool clip_limiter = true;
    double global_preamp_db = 0.0;
    ChannelCalibration left_cal{};
    ChannelCalibration right_cal{};
    std::vector<BandParams> bands;

    void clamp() {
        fir_fft_order = std::max(kMinFirFftOrder, std::min(kMaxFirFftOrder, fir_fft_order));
        global_preamp_db = clamp_double(global_preamp_db, kMinGainDb, kMaxGainDb);
        left_cal.clamp();
        right_cal.clamp();
        if (bands.size() > kMaxBands) {
            bands.resize(kMaxBands);
        }
        for (auto& b : bands) {
            b.clamp();
        }
    }

    bool has_lr_difference() const {
        if (left_cal != right_cal) {
            return true;
        }
        for (const auto& b : bands) {
            if (!b.enabled) continue;
            if (b.route == ChannelRoute::Left ||
                b.route == ChannelRoute::Right ||
                b.route == ChannelRoute::Side) {
                return true;
            }
        }
        return false;
    }

    bool has_ms_bands() const {
        for (const auto& b : bands) {
            if (!b.enabled) continue;
            if (b.route == ChannelRoute::Mid || b.route == ChannelRoute::Side) {
                return true;
            }
        }
        return false;
    }

    bool has_solo_bands() const {
        for (const auto& b : bands) {
            if (b.enabled && b.solo) {
                return true;
            }
        }
        return false;
    }

    void copy_channel_bands(ChannelRoute from, ChannelRoute to) {
        if (from == to) return;
        std::vector<BandParams> copied;
        for (const auto& b : bands) {
            if (b.route == from) {
                BandParams nb = b;
                nb.route = to;
                copied.push_back(nb);
            }
        }
        bands.erase(
            std::remove_if(bands.begin(), bands.end(),
                           [to](const BandParams& b) { return b.route == to; }),
            bands.end());
        for (const auto& nb : copied) {
            if (bands.size() < kMaxBands) {
                bands.push_back(nb);
            }
        }
    }

    void swap_lr() {
        for (auto& b : bands) {
            if (b.route == ChannelRoute::Left) {
                b.route = ChannelRoute::Right;
            } else if (b.route == ChannelRoute::Right) {
                b.route = ChannelRoute::Left;
            }
        }
        std::swap(left_cal, right_cal);
    }

    static EQPresetState create_default_10band() {
        EQPresetState st;
        st.name = "Default 10-Band Flat";
        const double freqs[10] = {31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
        for (double f : freqs) {
            BandParams b;
            b.enabled = true;
            b.type = FilterType::Bell;
            b.route = ChannelRoute::Stereo;
            b.slope = FilterSlope::dB12;
            b.freq_hz = f;
            b.gain_db = 0.0;
            b.q = 1.41421356;
            st.bands.push_back(b);
        }
        return st;
    }

    std::vector<uint8_t> serialize_binary() const {
        std::vector<uint8_t> buf;
        auto write_u8 = [&](uint8_t v) { buf.push_back(v); };
        auto write_u32 = [&](uint32_t v) {
            for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        };
        auto write_f64 = [&](double v) {
            uint64_t u = 0;
            std::memcpy(&u, &v, sizeof(u));
            for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((u >> (i * 8)) & 0xFF));
        };
        auto write_str = [&](const std::string& s) {
            uint32_t len = static_cast<uint32_t>(std::min<size_t>(s.size(), 4096));
            write_u32(len);
            buf.insert(buf.end(), s.begin(), s.begin() + len);
        };

        // Magic "SPEQ" + version 1
        write_u8('S');
        write_u8('P');
        write_u8('E');
        write_u8('Q');
        write_u32(1);

        write_str(name);
        write_u8(global_bypass ? 1 : 0);
        write_u8(link_lr ? 1 : 0);
        write_u8(static_cast<uint8_t>(processing_mode));
        write_u32(fir_fft_order);
        write_u8(auto_gain ? 1 : 0);
        write_u8(clip_limiter ? 1 : 0);
        write_f64(global_preamp_db);

        auto write_cal = [&](const ChannelCalibration& cal) {
            write_f64(cal.preamp_db);
            write_f64(cal.delay_ms);
            write_u8(cal.invert_phase ? 1 : 0);
            write_u8(cal.mute ? 1 : 0);
        };
        write_cal(left_cal);
        write_cal(right_cal);

        uint32_t count = static_cast<uint32_t>(std::min(bands.size(), kMaxBands));
        write_u32(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& b = bands[i];
            write_u8(b.enabled ? 1 : 0);
            write_u8(b.solo ? 1 : 0);
            write_u8(static_cast<uint8_t>(b.type));
            write_u8(static_cast<uint8_t>(b.route));
            write_u8(static_cast<uint8_t>(b.slope));
            write_f64(b.freq_hz);
            write_f64(b.gain_db);
            write_f64(b.q);
        }
        return buf;
    }

    static bool deserialize_binary(const uint8_t* data, size_t size, EQPresetState& out) {
        if (!data || size < 8) return false;
        size_t pos = 0;
        auto read_u8 = [&](uint8_t& v) -> bool {
            if (pos + 1 > size) return false;
            v = data[pos++];
            return true;
        };
        auto read_u32 = [&](uint32_t& v) -> bool {
            if (pos + 4 > size) return false;
            v = 0;
            for (int i = 0; i < 4; ++i) {
                v |= (static_cast<uint32_t>(data[pos++]) << (i * 8));
            }
            return true;
        };
        auto read_f64 = [&](double& v) -> bool {
            if (pos + 8 > size) return false;
            uint64_t u = 0;
            for (int i = 0; i < 8; ++i) {
                u |= (static_cast<uint64_t>(data[pos++]) << (i * 8));
            }
            std::memcpy(&v, &u, sizeof(v));
            return std::isfinite(v);
        };
        auto read_str = [&](std::string& s) -> bool {
            uint32_t len = 0;
            if (!read_u32(len) || len > 4096 || pos + len > size) return false;
            s.assign(reinterpret_cast<const char*>(data + pos), len);
            pos += len;
            return true;
        };

        if (data[0] != 'S' || data[1] != 'P' || data[2] != 'E' || data[3] != 'Q') {
            return false;
        }
        pos = 4;
        uint32_t version = 0;
        if (!read_u32(version) || version != 1) return false;

        EQPresetState st;
        if (!read_str(st.name)) return false;

        uint8_t btmp = 0;
        if (!read_u8(btmp)) return false;
        st.global_bypass = (btmp != 0);
        if (!read_u8(btmp)) return false;
        st.link_lr = (btmp != 0);
        if (!read_u8(btmp)) return false;
        st.processing_mode = (btmp == 1) ? ProcessingMode::LinearPhaseFIR : ProcessingMode::MinimumPhaseIIR;
        if (!read_u32(st.fir_fft_order)) return false;
        if (!read_u8(btmp)) return false;
        st.auto_gain = (btmp != 0);
        if (!read_u8(btmp)) return false;
        st.clip_limiter = (btmp != 0);
        if (!read_f64(st.global_preamp_db)) return false;

        auto read_cal = [&](ChannelCalibration& cal) -> bool {
            if (!read_f64(cal.preamp_db)) return false;
            if (!read_f64(cal.delay_ms)) return false;
            if (!read_u8(btmp)) return false;
            cal.invert_phase = (btmp != 0);
            if (!read_u8(btmp)) return false;
            cal.mute = (btmp != 0);
            return true;
        };
        if (!read_cal(st.left_cal) || !read_cal(st.right_cal)) return false;

        uint32_t count = 0;
        if (!read_u32(count) || count > kMaxBands) return false;
        st.bands.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto& b = st.bands[i];
            if (!read_u8(btmp)) return false;
            b.enabled = (btmp != 0);
            if (!read_u8(btmp)) return false;
            b.solo = (btmp != 0);
            if (!read_u8(btmp) || btmp > static_cast<uint8_t>(FilterType::FlatTilt)) return false;
            b.type = static_cast<FilterType>(btmp);
            if (!read_u8(btmp) || btmp > static_cast<uint8_t>(ChannelRoute::Side)) return false;
            b.route = static_cast<ChannelRoute>(btmp);
            if (!read_u8(btmp)) return false;
            switch (btmp) {
                case 1: b.slope = FilterSlope::dB6; break;
                case 2: b.slope = FilterSlope::dB12; break;
                case 3: b.slope = FilterSlope::dB18; break;
                case 4: b.slope = FilterSlope::dB24; break;
                case 6: b.slope = FilterSlope::dB36; break;
                case 8: b.slope = FilterSlope::dB48; break;
                default: return false;
            }
            if (!read_f64(b.freq_hz)) return false;
            if (!read_f64(b.gain_db)) return false;
            if (!read_f64(b.q)) return false;
        }
        st.clamp();
        out = std::move(st);
        return true;
    }
};

inline const char* filter_type_name(FilterType t) {
    switch (t) {
        case FilterType::Bell: return "Bell";
        case FilterType::LowShelf: return "Low Shelf";
        case FilterType::HighShelf: return "High Shelf";
        case FilterType::LowPass: return "Low Pass";
        case FilterType::HighPass: return "High Pass";
        case FilterType::Notch: return "Notch";
        case FilterType::BandPass: return "Band Pass";
        case FilterType::AllPass: return "All-Pass";
        case FilterType::FlatTilt: return "Flat Tilt";
    }
    return "Bell";
}

inline const char* channel_route_name(ChannelRoute r) {
    switch (r) {
        case ChannelRoute::Stereo: return "Stereo";
        case ChannelRoute::Left: return "Left";
        case ChannelRoute::Right: return "Right";
        case ChannelRoute::Mid: return "Mid";
        case ChannelRoute::Side: return "Side";
    }
    return "Stereo";
}

inline const char* filter_slope_name(FilterSlope s) {
    switch (s) {
        case FilterSlope::dB6: return "6 dB/oct";
        case FilterSlope::dB12: return "12 dB/oct";
        case FilterSlope::dB18: return "18 dB/oct";
        case FilterSlope::dB24: return "24 dB/oct";
        case FilterSlope::dB36: return "36 dB/oct";
        case FilterSlope::dB48: return "48 dB/oct";
    }
    return "12 dB/oct";
}

} // namespace super_eq
