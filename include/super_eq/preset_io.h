#pragma once

#include "types.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace super_eq {

class PresetIO {
public:
    static bool import_apo_text(const std::string& text,
                                EQPresetState& state,
                                ChannelRoute target_override = ChannelRoute::Stereo,
                                bool append = false) {
        EQPresetState result = append ? state : EQPresetState{};
        if (!append) {
            result.name = "Imported APO Preset";
        }
        ChannelRoute active_channel = (target_override != ChannelRoute::Stereo)
                                          ? target_override
                                          : ChannelRoute::Stereo;
        const bool force_channel = (target_override != ChannelRoute::Stereo);

        std::istringstream iss(text);
        std::string line;
        size_t parsed_items = 0;

        while (std::getline(iss, line)) {
            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
                continue;
            }
            std::string lower = to_lower(trimmed);

            if (starts_with(lower, "channel:")) {
                if (!force_channel) {
                    std::string ch = to_lower(trim(trimmed.substr(8)));
                    if (ch == "l" || ch == "left" || ch == "1" || ch == "fl") {
                        active_channel = ChannelRoute::Left;
                    } else if (ch == "r" || ch == "right" || ch == "2" || ch == "fr") {
                        active_channel = ChannelRoute::Right;
                    } else if (ch == "m" || ch == "mid") {
                        active_channel = ChannelRoute::Mid;
                    } else if (ch == "s" || ch == "side") {
                        active_channel = ChannelRoute::Side;
                    } else {
                        active_channel = ChannelRoute::Stereo;
                    }
                }
                continue;
            }

            if (starts_with(lower, "preamp:")) {
                double val = 0.0;
                if (parse_first_number(trimmed.substr(7), val)) {
                    if (active_channel == ChannelRoute::Left) {
                        result.left_cal.preamp_db = clamp_double(val, kMinGainDb, kMaxGainDb);
                    } else if (active_channel == ChannelRoute::Right) {
                        result.right_cal.preamp_db = clamp_double(val, kMinGainDb, kMaxGainDb);
                    } else {
                        result.global_preamp_db = clamp_double(val, kMinGainDb, kMaxGainDb);
                    }
                    ++parsed_items;
                }
                continue;
            }

            if (starts_with(lower, "filter")) {
                auto colon = trimmed.find(':');
                if (colon == std::string::npos) continue;
                std::string rhs = trim(trimmed.substr(colon + 1));
                std::vector<std::string> tokens = split_ws(rhs);
                if (tokens.size() < 4) continue;

                size_t idx = 0;
                BandParams band;
                band.route = active_channel;
                band.slope = FilterSlope::dB12;

                std::string state_tok = to_lower(tokens[idx++]);
                if (state_tok == "on") {
                    band.enabled = true;
                } else if (state_tok == "off") {
                    band.enabled = false;
                } else {
                    continue;
                }

                if (idx >= tokens.size()) continue;
                std::string type_tok = to_lower(tokens[idx++]);
                if (type_tok == "pk" || type_tok == "peq") {
                    band.type = FilterType::Bell;
                } else if (type_tok == "ls" || type_tok == "lsc") {
                    band.type = FilterType::LowShelf;
                } else if (type_tok == "hs" || type_tok == "hsc") {
                    band.type = FilterType::HighShelf;
                } else if (type_tok == "lp" || type_tok == "lpq") {
                    band.type = FilterType::LowPass;
                } else if (type_tok == "hp" || type_tok == "hpq") {
                    band.type = FilterType::HighPass;
                } else if (type_tok == "no") {
                    band.type = FilterType::Notch;
                } else if (type_tok == "bp") {
                    band.type = FilterType::BandPass;
                } else if (type_tok == "ap") {
                    band.type = FilterType::AllPass;
                } else if (type_tok == "none") {
                    continue;
                } else {
                    continue;
                }

                // Handle optional "6dB" / "12dB" token on LS/HS
                if (idx < tokens.size() && to_lower(tokens[idx]) == "6db") {
                    band.slope = FilterSlope::dB6;
                    ++idx;
                } else if (idx < tokens.size() && to_lower(tokens[idx]) == "12db") {
                    band.slope = FilterSlope::dB12;
                    ++idx;
                }

                bool has_fc = false;
                while (idx < tokens.size()) {
                    std::string key = to_lower(tokens[idx]);
                    if (key == "fc" && idx + 1 < tokens.size()) {
                        double v = 0.0;
                        if (parse_double(tokens[idx + 1], v)) {
                            band.freq_hz = v;
                            has_fc = true;
                        }
                        idx += 2;
                    } else if (key == "gain" && idx + 1 < tokens.size()) {
                        double v = 0.0;
                        if (parse_double(tokens[idx + 1], v)) {
                            band.gain_db = v;
                        }
                        idx += 2;
                    } else if (key == "q" && idx + 1 < tokens.size()) {
                        double v = 0.0;
                        if (parse_double(tokens[idx + 1], v)) {
                            band.q = v;
                        }
                        idx += 2;
                    } else if (key == "bw" && idx + 2 < tokens.size() && to_lower(tokens[idx + 1]) == "oct") {
                        double bw = 1.0;
                        if (parse_double(tokens[idx + 2], bw) && bw > 1e-4) {
                            double p = std::pow(2.0, bw);
                            band.q = std::sqrt(p) / (p - 1.0);
                        }
                        idx += 3;
                    } else {
                        ++idx;
                    }
                }

                if (has_fc && result.bands.size() < kMaxBands) {
                    band.clamp();
                    result.bands.push_back(band);
                    ++parsed_items;
                }
            }
        }

        if (parsed_items == 0) {
            return false;
        }
        result.clamp();
        state = std::move(result);
        return true;
    }

    static std::string export_apo_text(const EQPresetState& state) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "# foo_super_eq Equalizer APO Export: " << state.name << "\n";

        auto write_filter = [&](int num, const BandParams& b) {
            oss << "Filter " << num << ": " << (b.enabled ? "ON " : "OFF ");
            switch (b.type) {
                case FilterType::Bell: oss << "PK "; break;
                case FilterType::LowShelf: oss << "LSC "; break;
                case FilterType::HighShelf: oss << "HSC "; break;
                case FilterType::LowPass: oss << "LPQ "; break;
                case FilterType::HighPass: oss << "HPQ "; break;
                case FilterType::Notch: oss << "NO "; break;
                case FilterType::BandPass: oss << "BP "; break;
                case FilterType::AllPass: oss << "AP "; break;
                case FilterType::FlatTilt: oss << "PK "; break;
            }
            oss << "Fc " << std::setprecision(2) << b.freq_hz << " Hz ";
            if (b.type == FilterType::Bell || b.type == FilterType::LowShelf ||
                b.type == FilterType::HighShelf || b.type == FilterType::FlatTilt) {
                oss << "Gain " << std::setprecision(3) << b.gain_db << " dB ";
            }
            oss << "Q " << std::setprecision(4) << b.q << "\n";
        };

        // Stereo section
        oss << "Channel: all\n";
        if (std::abs(state.global_preamp_db) > 1e-6) {
            oss << "Preamp: " << std::setprecision(3) << state.global_preamp_db << " dB\n";
        }
        int idx = 1;
        for (const auto& b : state.bands) {
            if (b.route == ChannelRoute::Stereo) {
                write_filter(idx++, b);
            }
        }

        // Left section
        bool has_l = std::abs(state.left_cal.preamp_db) > 1e-6;
        for (const auto& b : state.bands) {
            if (b.route == ChannelRoute::Left) { has_l = true; break; }
        }
        if (has_l) {
            oss << "Channel: L\n";
            if (std::abs(state.left_cal.preamp_db) > 1e-6) {
                oss << "Preamp: " << std::setprecision(3) << state.left_cal.preamp_db << " dB\n";
            }
            idx = 1;
            for (const auto& b : state.bands) {
                if (b.route == ChannelRoute::Left) {
                    write_filter(idx++, b);
                }
            }
        }

        // Right section
        bool has_r = std::abs(state.right_cal.preamp_db) > 1e-6;
        for (const auto& b : state.bands) {
            if (b.route == ChannelRoute::Right) { has_r = true; break; }
        }
        if (has_r) {
            oss << "Channel: R\n";
            if (std::abs(state.right_cal.preamp_db) > 1e-6) {
                oss << "Preamp: " << std::setprecision(3) << state.right_cal.preamp_db << " dB\n";
            }
            idx = 1;
            for (const auto& b : state.bands) {
                if (b.route == ChannelRoute::Right) {
                    write_filter(idx++, b);
                }
            }
        }

        return oss.str();
    }

    static bool import_feq_text(const std::string& text,
                                EQPresetState& state,
                                ChannelRoute target_route = ChannelRoute::Stereo) {
        static constexpr double kFeqFreqs[18] = {
            55.0, 77.0, 110.0, 156.0, 220.0, 311.0,
            440.0, 622.0, 880.0, 1200.0, 1800.0, 2500.0,
            3500.0, 5000.0, 7000.0, 10000.0, 14000.0, 20000.0
        };
        std::istringstream iss(text);
        std::string line;
        std::vector<double> gains;
        while (std::getline(iss, line)) {
            std::string t = trim(line);
            if (t.empty()) continue;
            double val = 0.0;
            if (!parse_double(t, val)) {
                return false;
            }
            gains.push_back(val);
            if (gains.size() == 18) break;
        }
        if (gains.size() != 18) return false;

        EQPresetState result;
        result.name = "Imported FEQ Preset";
        for (size_t i = 0; i < 18; ++i) {
            BandParams b;
            b.enabled = true;
            b.type = FilterType::Bell;
            b.route = target_route;
            b.slope = FilterSlope::dB12;
            b.freq_hz = kFeqFreqs[i];
            b.gain_db = clamp_double(gains[i], kMinGainDb, kMaxGainDb);
            b.q = 2.8;
            result.bands.push_back(b);
        }
        result.clamp();
        state = std::move(result);
        return true;
    }

    static std::string to_json(const EQPresetState& state) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);
        auto write_cal = [&](const ChannelCalibration& cal, const char* indent) {
            oss << "{\n"
                << indent << "  \"preamp_db\": " << cal.preamp_db << ",\n"
                << indent << "  \"delay_ms\": " << cal.delay_ms << ",\n"
                << indent << "  \"invert_phase\": " << (cal.invert_phase ? "true" : "false") << ",\n"
                << indent << "  \"mute\": " << (cal.mute ? "true" : "false") << "\n"
                << indent << "}";
        };
        oss << "{\n"
            << "  \"format\": \"foo_super_eq\",\n"
            << "  \"version\": 1,\n"
            << "  \"name\": \"" << escape_json(state.name) << "\",\n"
            << "  \"global_bypass\": " << (state.global_bypass ? "true" : "false") << ",\n"
            << "  \"link_lr\": " << (state.link_lr ? "true" : "false") << ",\n"
            << "  \"processing_mode\": " << static_cast<int>(state.processing_mode) << ",\n"
            << "  \"fir_fft_order\": " << state.fir_fft_order << ",\n"
            << "  \"auto_gain\": " << (state.auto_gain ? "true" : "false") << ",\n"
            << "  \"clip_limiter\": " << (state.clip_limiter ? "true" : "false") << ",\n"
            << "  \"global_preamp_db\": " << state.global_preamp_db << ",\n"
            << "  \"left_cal\": ";
        write_cal(state.left_cal, "  ");
        oss << ",\n  \"right_cal\": ";
        write_cal(state.right_cal, "  ");
        oss << ",\n  \"bands\": [\n";
        for (size_t i = 0; i < state.bands.size(); ++i) {
            const auto& b = state.bands[i];
            oss << "    {\n"
                << "      \"enabled\": " << (b.enabled ? "true" : "false") << ",\n"
                << "      \"solo\": " << (b.solo ? "true" : "false") << ",\n"
                << "      \"type\": " << static_cast<int>(b.type) << ",\n"
                << "      \"route\": " << static_cast<int>(b.route) << ",\n"
                << "      \"slope\": " << static_cast<int>(b.slope) << ",\n"
                << "      \"freq_hz\": " << b.freq_hz << ",\n"
                << "      \"gain_db\": " << b.gain_db << ",\n"
                << "      \"q\": " << b.q << "\n"
                << "    }" << (i + 1 < state.bands.size() ? "," : "") << "\n";
        }
        oss << "  ]\n}\n";
        return oss.str();
    }

    static bool from_json(const std::string& json, EQPresetState& out) {
        JsonParser parser(json);
        JsonValue root;
        if (!parser.parse_value(root) || root.type != JsonValue::Object) {
            return false;
        }

        EQPresetState st;
        if (const auto* v = root.find("name")) {
            if (v->type == JsonValue::String) st.name = v->str_val;
        }
        if (const auto* v = root.find("global_bypass")) st.global_bypass = v->as_bool(false);
        if (const auto* v = root.find("link_lr")) st.link_lr = v->as_bool(false);
        if (const auto* v = root.find("processing_mode")) {
            st.processing_mode = (v->as_int(0) == 1) ? ProcessingMode::LinearPhaseFIR
                                                     : ProcessingMode::MinimumPhaseIIR;
        }
        if (const auto* v = root.find("fir_fft_order")) {
            st.fir_fft_order = static_cast<uint32_t>(v->as_int(kDefaultFirFftOrder));
        }
        if (const auto* v = root.find("auto_gain")) st.auto_gain = v->as_bool(false);
        if (const auto* v = root.find("clip_limiter")) st.clip_limiter = v->as_bool(true);
        if (const auto* v = root.find("global_preamp_db")) st.global_preamp_db = v->as_double(0.0);

        auto read_cal = [&](const char* key, ChannelCalibration& cal) {
            const auto* obj = root.find(key);
            if (!obj || obj->type != JsonValue::Object) return;
            if (const auto* v = obj->find("preamp_db")) cal.preamp_db = v->as_double(0.0);
            if (const auto* v = obj->find("delay_ms")) cal.delay_ms = v->as_double(0.0);
            if (const auto* v = obj->find("invert_phase")) cal.invert_phase = v->as_bool(false);
            if (const auto* v = obj->find("mute")) cal.mute = v->as_bool(false);
        };
        read_cal("left_cal", st.left_cal);
        read_cal("right_cal", st.right_cal);

        if (const auto* arr = root.find("bands")) {
            if (arr->type != JsonValue::Array) return false;
            for (const auto& item : arr->arr_val) {
                if (item.type != JsonValue::Object) continue;
                if (st.bands.size() >= kMaxBands) break;
                BandParams b;
                if (const auto* v = item.find("enabled")) b.enabled = v->as_bool(true);
                if (const auto* v = item.find("solo")) b.solo = v->as_bool(false);
                if (const auto* v = item.find("type")) {
                    int t = v->as_int(0);
                    if (t >= 0 && t <= static_cast<int>(FilterType::FlatTilt)) {
                        b.type = static_cast<FilterType>(t);
                    }
                }
                if (const auto* v = item.find("route")) {
                    int r = v->as_int(0);
                    if (r >= 0 && r <= static_cast<int>(ChannelRoute::Side)) {
                        b.route = static_cast<ChannelRoute>(r);
                    }
                }
                if (const auto* v = item.find("slope")) {
                    int s = v->as_int(2);
                    switch (s) {
                        case 1: b.slope = FilterSlope::dB6; break;
                        case 2: b.slope = FilterSlope::dB12; break;
                        case 3: b.slope = FilterSlope::dB18; break;
                        case 4: b.slope = FilterSlope::dB24; break;
                        case 6: b.slope = FilterSlope::dB36; break;
                        case 8: b.slope = FilterSlope::dB48; break;
                        default: b.slope = FilterSlope::dB12; break;
                    }
                }
                if (const auto* v = item.find("freq_hz")) b.freq_hz = v->as_double(1000.0);
                if (const auto* v = item.find("gain_db")) b.gain_db = v->as_double(0.0);
                if (const auto* v = item.find("q")) b.q = v->as_double(0.70710678);
                b.clamp();
                st.bands.push_back(b);
            }
        }
        st.clamp();
        out = std::move(st);
        return true;
    }

private:
    static std::string trim(const std::string& s) {
        size_t a = 0;
        while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        size_t b = s.size();
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }

    static std::string to_lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    static bool starts_with(const std::string& s, const std::string& prefix) {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    static std::vector<std::string> split_ws(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream iss(s);
        std::string tok;
        while (iss >> tok) out.push_back(tok);
        return out;
    }

    static bool parse_double(const std::string& s, double& val) {
        try {
            size_t pos = 0;
            val = std::stod(s, &pos);
            return pos > 0 && std::isfinite(val);
        } catch (...) {
            return false;
        }
    }

    static bool parse_first_number(const std::string& s, double& val) {
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
                return parse_double(s.substr(i), val);
            }
        }
        return false;
    }

    static std::string escape_json(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '\"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out.push_back(c); break;
            }
        }
        return out;
    }

    struct JsonValue {
        enum Type { Null, Bool, Number, String, Array, Object } type = Null;
        bool bool_val = false;
        double num_val = 0.0;
        std::string str_val;
        std::vector<JsonValue> arr_val;
        std::vector<std::pair<std::string, JsonValue>> obj_val;

        const JsonValue* find(const char* key) const {
            if (type != Object) return nullptr;
            for (const auto& kv : obj_val) {
                if (kv.first == key) return &kv.second;
            }
            return nullptr;
        }
        bool as_bool(bool def) const { return (type == Bool) ? bool_val : def; }
        double as_double(double def) const { return (type == Number) ? num_val : def; }
        int as_int(int def) const { return (type == Number) ? static_cast<int>(std::llround(num_val)) : def; }
    };

    class JsonParser {
    public:
        explicit JsonParser(const std::string& src) : src_(src) {}

        bool parse_value(JsonValue& out) {
            skip_ws();
            if (pos_ >= src_.size()) return false;
            char c = src_[pos_];
            if (c == '{') return parse_object(out);
            if (c == '[') return parse_array(out);
            if (c == '"') {
                out.type = JsonValue::String;
                return parse_string(out.str_val);
            }
            if (c == 't' && src_.compare(pos_, 4, "true") == 0) {
                pos_ += 4;
                out.type = JsonValue::Bool;
                out.bool_val = true;
                return true;
            }
            if (c == 'f' && src_.compare(pos_, 5, "false") == 0) {
                pos_ += 5;
                out.type = JsonValue::Bool;
                out.bool_val = false;
                return true;
            }
            if (c == 'n' && src_.compare(pos_, 4, "null") == 0) {
                pos_ += 4;
                out.type = JsonValue::Null;
                return true;
            }
            if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
                return parse_number(out);
            }
            return false;
        }

    private:
        void skip_ws() {
            while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) {
                ++pos_;
            }
        }

        bool parse_string(std::string& out) {
            if (pos_ >= src_.size() || src_[pos_] != '"') return false;
            ++pos_;
            out.clear();
            while (pos_ < src_.size()) {
                char c = src_[pos_++];
                if (c == '"') return true;
                if (c == '\\') {
                    if (pos_ >= src_.size()) return false;
                    char esc = src_[pos_++];
                    switch (esc) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        default: out.push_back(esc); break;
                    }
                } else {
                    out.push_back(c);
                }
            }
            return false;
        }

        bool parse_number(JsonValue& out) {
            size_t start = pos_;
            if (src_[pos_] == '-') ++pos_;
            while (pos_ < src_.size() &&
                   (std::isdigit(static_cast<unsigned char>(src_[pos_])) ||
                    src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E' ||
                    src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            double v = 0.0;
            if (!parse_double(src_.substr(start, pos_ - start), v)) return false;
            out.type = JsonValue::Number;
            out.num_val = v;
            return true;
        }

        bool parse_array(JsonValue& out) {
            ++pos_;
            out.type = JsonValue::Array;
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (pos_ < src_.size()) {
                JsonValue elem;
                if (!parse_value(elem)) return false;
                out.arr_val.push_back(std::move(elem));
                skip_ws();
                if (pos_ >= src_.size()) return false;
                if (src_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                if (src_[pos_] != ',') return false;
                ++pos_;
            }
            return false;
        }

        bool parse_object(JsonValue& out) {
            ++pos_;
            out.type = JsonValue::Object;
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (pos_ < src_.size()) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) return false;
                skip_ws();
                if (pos_ >= src_.size() || src_[pos_] != ':') return false;
                ++pos_;
                JsonValue val;
                if (!parse_value(val)) return false;
                out.obj_val.emplace_back(std::move(key), std::move(val));
                skip_ws();
                if (pos_ >= src_.size()) return false;
                if (src_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                if (src_[pos_] != ',') return false;
                ++pos_;
            }
            return false;
        }

        const std::string& src_;
        size_t pos_ = 0;
    };
};

class ABStateManager {
public:
    ABStateManager() {
        slots_[0] = EQPresetState::create_default_10band();
        slots_[1] = slots_[0];
        slots_[1].name = "Slot B";
    }

    int active_slot() const { return active_slot_; }

    EQPresetState& current() { return slots_[active_slot_]; }
    const EQPresetState& current() const { return slots_[active_slot_]; }

    EQPresetState& slot(int idx) { return slots_[idx & 1]; }
    const EQPresetState& slot(int idx) const { return slots_[idx & 1]; }

    void set_active_slot(int idx) { active_slot_ = (idx & 1); }

    int switch_slot() {
        active_slot_ = 1 - active_slot_;
        return active_slot_;
    }

    void copy_current_to_other() {
        int other = 1 - active_slot_;
        slots_[other] = slots_[active_slot_];
    }

private:
    EQPresetState slots_[2];
    int active_slot_ = 0;
};

} // namespace super_eq
