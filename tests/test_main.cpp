#include "super_eq/biquad.h"
#include "super_eq/dsp_engine.h"
#include "super_eq/fft.h"
#include "super_eq/fir_convolver.h"
#include "super_eq/preset_io.h"
#include "super_eq/spectrum_analyzer.h"
#include "super_eq/types.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using namespace super_eq;

namespace {

int g_tests_passed = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        std::exit(1);
    }
}

bool near(double a, double b, double tol) {
    return std::abs(a - b) <= tol;
}

double measure_sine_rms_db(BiquadCascade& cas, double freq_hz, double sample_rate) {
    cas.reset();
    const size_t total = 16384;
    const size_t settle = 8192;
    double sum_sq = 0.0;
    for (size_t i = 0; i < total; ++i) {
        double x = 0.5 * std::sin(2.0 * kPi * freq_hz * static_cast<double>(i) / sample_rate);
        double y = cas.process(x);
        if (i >= settle) {
            sum_sq += y * y;
        }
    }
    double out_rms = std::sqrt(sum_sq / static_cast<double>(total - settle));
    double in_rms = 0.5 / std::sqrt(2.0);
    return linear_to_db(out_rms / in_rms);
}

void test_biquad_types_and_slopes() {
    const double fs = 48000.0;

    // 1. Bell filter: +6 dB at 1 kHz, Q=2.0 -> +6.0 dB at 1 kHz, ~0 dB at 50 Hz and 15 kHz
    {
        BandParams b;
        b.type = FilterType::Bell;
        b.freq_hz = 1000.0;
        b.gain_db = 6.0;
        b.q = 2.0;
        auto cas = BiquadCascade::design(b, fs);
        check(near(cas.magnitude_db(1000.0, fs), 6.0, 0.01), "Bell 1kHz transfer peak");
        check(near(cas.magnitude_db(50.0, fs), 0.0, 0.05), "Bell 50Hz far off-center");
        check(near(measure_sine_rms_db(cas, 1000.0, fs), 6.0, 0.05), "Bell 1kHz time-domain RMS");
    }

    // 2. LowShelf & HighShelf across slopes (6, 12, 24, 48 dB/oct)
    {
        const FilterSlope slopes[] = {FilterSlope::dB6, FilterSlope::dB12, FilterSlope::dB24, FilterSlope::dB48};
        for (auto s : slopes) {
            BandParams ls;
            ls.type = FilterType::LowShelf;
            ls.slope = s;
            ls.freq_hz = 200.0;
            ls.gain_db = -8.0;
            ls.q = 0.70710678;
            auto cas_ls = BiquadCascade::design(ls, fs);
            check(near(cas_ls.magnitude_db(20.0, fs), -8.0, 0.25), "LowShelf DC gain");
            check(near(cas_ls.magnitude_db(10000.0, fs), 0.0, 0.15), "LowShelf HF passband");

            BandParams hs;
            hs.type = FilterType::HighShelf;
            hs.slope = s;
            hs.freq_hz = 4000.0;
            hs.gain_db = 5.0;
            hs.q = 0.70710678;
            auto cas_hs = BiquadCascade::design(hs, fs);
            check(near(cas_hs.magnitude_db(40.0, fs), 0.0, 0.15), "HighShelf LF passband");
            check(near(cas_hs.magnitude_db(18000.0, fs), 5.0, 0.35), "HighShelf HF gain");
        }
    }

    // 3. LowPass & HighPass for all 6 slopes (6, 12, 18, 24, 36, 48 dB/oct)
    {
        const FilterSlope slopes[] = {
            FilterSlope::dB6, FilterSlope::dB12, FilterSlope::dB18,
            FilterSlope::dB24, FilterSlope::dB36, FilterSlope::dB48
        };
        const double expected_oct_db[6] = {-7.0, -12.0, -18.0, -24.0, -36.0, -48.0};
        for (size_t i = 0; i < 6; ++i) {
            BandParams lp;
            lp.type = FilterType::LowPass;
            lp.slope = slopes[i];
            lp.freq_hz = 1000.0;
            lp.q = 0.70710678;
            auto cas_lp = BiquadCascade::design(lp, fs);
            check(near(cas_lp.magnitude_db(1000.0, fs), -3.01, 0.15), "LowPass -3dB cutoff");
            double mag_2k = cas_lp.magnitude_db(2000.0, fs);
            check(mag_2k < expected_oct_db[i] + 2.5, "LowPass 1-octave attenuation slope");
            check(near(cas_lp.magnitude_db(100.0, fs), 0.0, 0.1), "LowPass passband");

            BandParams hp;
            hp.type = FilterType::HighPass;
            hp.slope = slopes[i];
            hp.freq_hz = 1000.0;
            hp.q = 0.70710678;
            auto cas_hp = BiquadCascade::design(hp, fs);
            check(near(cas_hp.magnitude_db(1000.0, fs), -3.01, 0.15), "HighPass -3dB cutoff");
            double mag_500 = cas_hp.magnitude_db(500.0, fs);
            check(mag_500 < expected_oct_db[i] + 2.5, "HighPass 1-octave attenuation slope");
            check(near(cas_hp.magnitude_db(10000.0, fs), 0.0, 0.1), "HighPass passband");
        }
    }

    // 4. Notch, BandPass, AllPass, FlatTilt
    {
        BandParams notch;
        notch.type = FilterType::Notch;
        notch.freq_hz = 2500.0;
        notch.q = 8.0;
        auto cas_n = BiquadCascade::design(notch, fs);
        check(cas_n.magnitude_db(2500.0, fs) < -40.0, "Notch deep null at fc");
        check(near(cas_n.magnitude_db(200.0, fs), 0.0, 0.05), "Notch passband");

        BandParams bp;
        bp.type = FilterType::BandPass;
        bp.freq_hz = 1500.0;
        bp.q = 4.0;
        auto cas_bp = BiquadCascade::design(bp, fs);
        check(near(cas_bp.magnitude_db(1500.0, fs), 0.0, 0.05), "BandPass 0 dB center");
        check(cas_bp.magnitude_db(150.0, fs) < -20.0, "BandPass LF rejection");
        check(cas_bp.magnitude_db(12000.0, fs) < -20.0, "BandPass HF rejection");

        BandParams ap;
        ap.type = FilterType::AllPass;
        ap.freq_hz = 1000.0;
        ap.q = 1.0;
        auto cas_ap = BiquadCascade::design(ap, fs);
        check(near(cas_ap.magnitude_db(1000.0, fs), 0.0, 1e-4), "AllPass unity magnitude at fc");
        check(near(cas_ap.magnitude_db(5000.0, fs), 0.0, 1e-4), "AllPass unity magnitude HF");
        check(std::abs(cas_ap.phase_deg(1000.0, fs)) > 90.0, "AllPass phase rotation");

        BandParams tilt;
        tilt.type = FilterType::FlatTilt;
        tilt.freq_hz = 1000.0;
        tilt.gain_db = 6.0;
        auto cas_t = BiquadCascade::design(tilt, fs);
        check(near(cas_t.magnitude_db(1000.0, fs), 0.0, 0.15), "FlatTilt 0 dB pivot at fc");
        check(cas_t.magnitude_db(50.0, fs) < -2.0, "FlatTilt cuts lows for positive tilt");
        check(cas_t.magnitude_db(16000.0, fs) > 2.0, "FlatTilt boosts highs for positive tilt");
    }

    std::cout << "[PASS] test_biquad_types_and_slopes\n";
    ++g_tests_passed;
}

void test_lr_ms_separation_and_mono_upmix() {
    const unsigned sr = 48000;
    const size_t frames = 16384;

    // 1. Left-only +6 dB at 1 kHz, Right-only -6 dB at 1 kHz
    EQPresetState st;
    st.clip_limiter = false;
    BandParams bl;
    bl.route = ChannelRoute::Left;
    bl.type = FilterType::Bell;
    bl.freq_hz = 1000.0;
    bl.gain_db = 6.0;
    bl.q = 1.4;
    BandParams br = bl;
    br.route = ChannelRoute::Right;
    br.gain_db = -6.0;
    st.bands = {bl, br};

    SuperEQEngine eng;
    eng.apply_state_now(st, sr);
    check(eng.should_upmix_mono_to_stereo(), "Mono upmix required when L/R differ");

    std::vector<float> buf(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        float s = 0.25f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / sr));
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
    eng.process_interleaved(buf.data(), frames, 2, sr);

    double sq_l = 0.0, sq_r = 0.0;
    for (size_t i = frames / 2; i < frames; ++i) {
        sq_l += buf[i * 2] * buf[i * 2];
        sq_r += buf[i * 2 + 1] * buf[i * 2 + 1];
    }
    double in_rms = 0.25 / std::sqrt(2.0);
    double db_l = linear_to_db(std::sqrt(sq_l / (frames / 2)) / in_rms);
    double db_r = linear_to_db(std::sqrt(sq_r / (frames / 2)) / in_rms);
    check(near(db_l, 6.0, 0.08), "Left channel receives +6 dB");
    check(near(db_r, -6.0, 0.08), "Right channel receives -6 dB");

    // 2. Mid / Side isolation test
    EQPresetState ms;
    ms.clip_limiter = false;
    BandParams b_side;
    b_side.route = ChannelRoute::Side;
    b_side.type = FilterType::Bell;
    b_side.freq_hz = 1000.0;
    b_side.gain_db = 6.0;
    b_side.q = 1.4;
    ms.bands = {b_side};
    eng.apply_state_now(ms, sr);

    // Pure Mid input (L == R): Side band must leave it untouched (0 dB)
    for (size_t i = 0; i < frames; ++i) {
        float s = 0.25f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / sr));
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
    eng.process_interleaved(buf.data(), frames, 2, sr);
    double mid_sq = 0.0;
    for (size_t i = frames / 2; i < frames; ++i) mid_sq += buf[i * 2] * buf[i * 2];
    double mid_db = linear_to_db(std::sqrt(mid_sq / (frames / 2)) / in_rms);
    check(near(mid_db, 0.0, 0.05), "Side EQ leaves pure Mid signal untouched");

    // Pure Side input (L == -R): Side band must boost by +6 dB
    for (size_t i = 0; i < frames; ++i) {
        float s = 0.25f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / sr));
        buf[i * 2] = s;
        buf[i * 2 + 1] = -s;
    }
    eng.process_interleaved(buf.data(), frames, 2, sr);
    double side_sq = 0.0;
    for (size_t i = frames / 2; i < frames; ++i) side_sq += buf[i * 2] * buf[i * 2];
    double side_db = linear_to_db(std::sqrt(side_sq / (frames / 2)) / in_rms);
    check(near(side_db, 6.0, 0.08), "Side EQ boosts pure Side signal by +6 dB");

    std::cout << "[PASS] test_lr_ms_separation_and_mono_upmix\n";
    ++g_tests_passed;
}

void test_linear_phase_fir_and_surround_latency() {
    const unsigned sr = 48000;

    // 1. Verify LinearPhaseFIRConvolver impulse symmetry and exact group delay
    BandParams bp;
    bp.type = FilterType::Bell;
    bp.freq_hz = 1000.0;
    bp.gain_db = 6.0;
    bp.q = 1.4;
    auto cas = BiquadCascade::design(bp, sr);

    LinearPhaseFIRConvolver conv;
    conv.configure([&](double f) { return std::abs(cas.eval_transfer(f, sr)); }, sr, 14);

    const auto& k = conv.kernel();
    check(k.size() == conv.num_taps() && (k.size() % 2 == 1), "FIR odd tap count");
    size_t mid = k.size() / 2;
    double max_asym = 0.0;
    for (size_t i = 0; i < mid; ++i) {
        max_asym = std::max(max_asym, std::abs(k[i] - k[k.size() - 1 - i]));
    }
    check(max_asym < 1e-12, "FIR impulse strictly symmetric (zero phase distortion)");

    // Measure impulse response delay through convolver
    const size_t lat = conv.latency_samples();
    size_t peak_idx = 0;
    double peak_val = -1.0;
    for (size_t i = 0; i < lat + 2048; ++i) {
        double out = conv.process_sample(i == 0 ? 1.0 : 0.0);
        if (std::abs(out) > peak_val) {
            peak_val = std::abs(out);
            peak_idx = i;
        }
    }
    check(peak_idx == lat, "FIR impulse peak occurs at exact latency_samples()");

    // Verify steady-state 1 kHz sine gain matches +6 dB
    conv.reset();
    const size_t frames = 32768;
    double sq_out = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        double x = 0.25 * std::sin(2.0 * kPi * 1000.0 * i / sr);
        double y = conv.process_sample(x);
        if (i >= 16384) sq_out += y * y;
    }
    double fir_db = linear_to_db(std::sqrt(sq_out / 16384.0) / (0.25 / std::sqrt(2.0)));
    check(near(fir_db, 6.0, 0.15), "Linear-Phase FIR 1 kHz sine gain matches +6 dB");

    // 2. Surround 5.1 (6 channels) latency alignment in Linear-Phase FIR mode
    EQPresetState st;
    st.processing_mode = ProcessingMode::LinearPhaseFIR;
    st.fir_fft_order = 12; // 4096 design FFT for fast test
    st.clip_limiter = false;
    BandParams flat;
    flat.gain_db = 0.0;
    st.bands = {flat};

    SuperEQEngine eng;
    eng.apply_state_now(st, sr);
    size_t eng_lat = eng.latency_samples();
    check(eng_lat > 0, "Engine reports non-zero latency in LinearPhaseFIR mode");

    const size_t n_frames = eng_lat + 1024;
    std::vector<float> surround(n_frames * 6, 0.0f);
    // Impulse on all 6 channels at frame 0
    for (size_t ch = 0; ch < 6; ++ch) surround[ch] = 1.0f;
    eng.process_interleaved(surround.data(), n_frames, 6, sr);

    for (size_t ch = 0; ch < 6; ++ch) {
        size_t p_idx = 0;
        float p_val = 0.0f;
        for (size_t f = 0; f < n_frames; ++f) {
            float v = std::abs(surround[f * 6 + ch]);
            if (v > p_val) {
                p_val = v;
                p_idx = f;
            }
        }
        check(p_idx == eng_lat, "Surround channel impulse aligns to exact FL/FR FIR latency");
        check(near(p_val, 1.0, 0.01), "Surround channel preserves impulse amplitude");
    }

    std::cout << "[PASS] test_linear_phase_fir_and_surround_latency (FIR lat=" << lat << ")\n";
    ++g_tests_passed;
}

void test_calibration_autogain_limiter() {
    const unsigned sr = 48000;
    EQPresetState st;
    st.auto_gain = true;
    st.clip_limiter = true;
    BandParams b;
    b.route = ChannelRoute::Left;
    b.type = FilterType::Bell;
    b.freq_hz = 1000.0;
    b.gain_db = 9.0;
    b.q = 1.0;
    st.bands = {b};

    double auto_g = SuperEQEngine::compute_auto_gain_db(st, sr);
    check(near(auto_g, -9.0, 0.1), "Auto-gain offsets peak +9 dB boost");

    // Delay + phase invert test
    EQPresetState cal_st;
    cal_st.clip_limiter = true;
    cal_st.left_cal.preamp_db = 6.0;
    cal_st.left_cal.invert_phase = true;
    cal_st.right_cal.delay_ms = 1.0; // exactly 48 samples at 48 kHz

    SuperEQEngine eng;
    eng.apply_state_now(cal_st, sr);
    std::vector<float> buf(256 * 2, 0.0f);
    buf[0] = 0.8f; // L impulse (0.8 * 2 = 1.6 -> soft limited < 1.0, inverted negative)
    buf[1] = 0.5f; // R impulse delayed by 48 samples
    eng.process_interleaved(buf.data(), 256, 2, sr);

    check(buf[0] < 0.0f && buf[0] > -1.0f, "Left polarity inverted & clip limiter keeps < 1.0");
    check(near(buf[1], 0.0, 1e-6), "Right sample 0 silent due to 1ms delay");
    check(near(buf[48 * 2 + 1], 0.5, 1e-5), "Right impulse arrives at exact 48-sample (1.0 ms) delay");

    std::cout << "[PASS] test_calibration_autogain_limiter\n";
    ++g_tests_passed;
}

void test_preset_io_and_ab() {
    // 1. Equalizer APO with Channel: L / Channel: R + BW Oct
    const std::string apo =
        "# REW Export\n"
        "Channel: all\n"
        "Preamp: -3.5 dB\n"
        "Filter 1: ON PK Fc 100 Hz Gain 4.0 dB BW Oct 1.0\n"
        "Channel: L\n"
        "Preamp: -1.0 dB\n"
        "Filter 1: ON LSC Fc 120 Hz Gain 3.0 dB Q 0.707\n"
        "Channel: R\n"
        "Filter 1: OFF HS Fc 8000 Hz Gain -2.5 dB Q 0.8\n";

    EQPresetState st;
    check(PresetIO::import_apo_text(apo, st), "Import multi-channel APO text");
    check(near(st.global_preamp_db, -3.5, 1e-4), "APO global preamp");
    check(near(st.left_cal.preamp_db, -1.0, 1e-4), "APO left preamp");
    check(st.bands.size() == 3, "APO band count == 3");
    check(st.bands[0].route == ChannelRoute::Stereo && near(st.bands[0].q, 1.41421356, 1e-3), "APO BW Oct -> Q");
    check(st.bands[1].route == ChannelRoute::Left && st.bands[1].type == FilterType::LowShelf, "APO Left LowShelf");
    check(st.bands[2].route == ChannelRoute::Right && !st.bands[2].enabled, "APO Right disabled HighShelf");

    // Force-override import into Right (append)
    const std::string rew_r = "Filter 1: ON PK Fc 2500 Hz Gain -4.0 dB Q 3.0\n";
    check(PresetIO::import_apo_text(rew_r, st, ChannelRoute::Right, true), "Append APO into Right");
    check(st.bands.size() == 4 && st.bands[3].route == ChannelRoute::Right, "Appended band routed to Right");

    // Export APO and re-import round-trip
    std::string exported = PresetIO::export_apo_text(st);
    EQPresetState re_apo;
    check(PresetIO::import_apo_text(exported, re_apo), "Re-import exported APO");
    check(re_apo.bands.size() == 4, "APO round-trip preserves 4 bands");

    // 2. JSON round-trip
    st.processing_mode = ProcessingMode::LinearPhaseFIR;
    st.left_cal.delay_ms = 2.5;
    st.right_cal.invert_phase = true;
    std::string json = PresetIO::to_json(st);
    EQPresetState from_j;
    check(PresetIO::from_json(json, from_j), "JSON parse");
    check(from_j.processing_mode == ProcessingMode::LinearPhaseFIR, "JSON preserves mode");
    check(near(from_j.left_cal.delay_ms, 2.5, 1e-5), "JSON preserves delay_ms");
    check(from_j.right_cal.invert_phase, "JSON preserves invert_phase");
    check(from_j.bands.size() == st.bands.size(), "JSON preserves band count");

    // 3. Binary dsp_preset round-trip
    auto blob = st.serialize_binary();
    EQPresetState from_bin;
    check(EQPresetState::deserialize_binary(blob.data(), blob.size(), from_bin), "Binary deserialize");
    check(from_bin.bands.size() == st.bands.size(), "Binary preserves band count");
    check(near(from_bin.bands[3].freq_hz, 2500.0, 1e-6), "Binary preserves freq");

    // 4. foobar2000 .feq import
    std::string feq;
    for (int i = 0; i < 18; ++i) feq += (i == 8 ? "5\n" : "0\n");
    EQPresetState feq_st;
    check(PresetIO::import_feq_text(feq, feq_st, ChannelRoute::Left), "FEQ import");
    check(feq_st.bands.size() == 18 && near(feq_st.bands[8].gain_db, 5.0, 1e-5), "FEQ band 8 == +5 dB");
    check(feq_st.bands[8].route == ChannelRoute::Left, "FEQ target route Left");

    // 5. A/B State Manager
    ABStateManager ab;
    ab.current().global_preamp_db = -2.0;
    ab.copy_current_to_other();
    ab.switch_slot();
    check(near(ab.current().global_preamp_db, -2.0, 1e-6), "A/B copy & switch");
    ab.current().global_preamp_db = 4.0;
    ab.switch_slot();
    check(near(ab.current().global_preamp_db, -2.0, 1e-6), "A/B independent slots");

    std::cout << "[PASS] test_preset_io_and_ab\n";
    ++g_tests_passed;
}

void test_spectrum_analyzer_and_ring_buffer() {
    AudioRingBuffer ring;
    const double sr = 48000.0;
    const size_t N = 4096;
    for (size_t i = 0; i < N; ++i) {
        float s1k = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / sr));
        float s4k = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 4000.0 * i / sr));
        ring.push(AudioAnalysisFrame{s1k, s4k, s1k, s4k});
    }
    check(ring.available() == N, "RingBuffer holds 4096 frames");

    SpectrumAnalyzer spec(4096, 256);
    spec.set_smoothing(0.0f);
    check(spec.update_from_ring(ring, sr), "SpectrumAnalyzer processes ring frames");

    const auto& freqs = spec.freqs();
    const auto& post_l = spec.post_left_db();
    const auto& post_r = spec.post_right_db();

    size_t peak_l_idx = 0, peak_r_idx = 0;
    for (size_t i = 1; i < freqs.size(); ++i) {
        if (post_l[i] > post_l[peak_l_idx]) peak_l_idx = i;
        if (post_r[i] > post_r[peak_r_idx]) peak_r_idx = i;
    }
    check(near(freqs[peak_l_idx], 1000.0, 60.0), "Spectrum Left peaks near 1 kHz");
    check(near(freqs[peak_r_idx], 4000.0, 200.0), "Spectrum Right peaks near 4 kHz");

    std::cout << "[PASS] test_spectrum_analyzer_and_ring_buffer\n";
    ++g_tests_passed;
}

} // namespace

int main() {
    test_biquad_types_and_slopes();
    test_lr_ms_separation_and_mono_upmix();
    test_linear_phase_fir_and_surround_latency();
    test_calibration_autogain_limiter();
    test_preset_io_and_ab();
    test_spectrum_analyzer_and_ring_buffer();
    std::cout << "\nAll " << g_tests_passed << " test suites passed.\n";
    return 0;
}
