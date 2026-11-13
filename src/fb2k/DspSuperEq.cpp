#include "Guids.h"
#include "LiveSessionBus.h"
#include "SuperEqEditorView.h"
#include <foobar2000/SDK/foobar2000.h>
#include <helpers/foobar2000+atl.h>
#include <vector>

namespace super_eq_fb2k {

class SuperEqPopupDialog : public ATL::CWindowImpl<SuperEqPopupDialog> {
public:
    DECLARE_WND_CLASS_EX(_T("SuperEqPopupDialogWnd"), 0, COLOR_BTNFACE)

    SuperEqPopupDialog(const super_eq::EQPresetState& init_state, dsp_preset_edit_callback& cb)
        : initial_state_(init_state), callback_(cb) {}

    BEGIN_MSG_MAP(SuperEqPopupDialog)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
    END_MSG_MAP()

    void run_modal(HWND parent) {
        RECT rc{0, 0, 980, 620};
        Create(parent, rc, _T("Super EQ — Left/Right Parametric & Linear-Phase FIR"),
               WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN, 0);
        CenterWindow(parent);
        ShowWindow(SW_SHOW);
        UpdateWindow();

        if (parent) ::EnableWindow(parent, FALSE);
        MSG msg{};
        while (!closed_ && ::IsWindow(m_hWnd)) {
            BOOL ret = ::GetMessageW(&msg, nullptr, 0, 0);
            if (ret <= 0) {
                if (ret == 0) ::PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (!::IsDialogMessageW(m_hWnd, &msg)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
        }
        if (parent) {
            ::EnableWindow(parent, TRUE);
            ::SetActiveWindow(parent);
        }
    }

private:
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
        view_.set_popup_callback(&callback_);
        view_.set_initial_state(initial_state_);
        RECT rc{};
        GetClientRect(&rc);
        view_.Create(m_hWnd, rc, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN);
        dark_mode_.AddDialogWithControls(m_hWnd);
        return 0;
    }

    LRESULT OnSize(UINT, WPARAM, LPARAM lParam, BOOL&) {
        if (view_.m_hWnd) {
            view_.MoveWindow(0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    }

    LRESULT OnGetMinMaxInfo(UINT, WPARAM, LPARAM lParam, BOOL&) {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 780;
        mmi->ptMinTrackSize.y = 460;
        return 0;
    }

    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&) {
        DestroyWindow();
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&) {
        closed_ = true;
        ::PostMessageW(nullptr, WM_NULL, 0, 0);
        return 0;
    }

    bool closed_ = false;
    super_eq::EQPresetState initial_state_;
    dsp_preset_edit_callback& callback_;
    SuperEqEditorView view_;
    fb2k::CCoreDarkModeHooks dark_mode_;
};

class dsp_super_eq : public dsp_impl_base_t<dsp_v2> {
public:
    explicit dsp_super_eq(dsp_preset const& in) {
        super_eq::EQPresetState st = super_eq::EQPresetState::create_default_10band();
        LiveSessionBus::preset_to_state(in, st);
        engine_.apply_state_now(st, 44100.0);
        LiveSessionBus::instance().register_engine(&engine_, st);
    }

    ~dsp_super_eq() override {
        LiveSessionBus::instance().unregister_engine(&engine_);
    }

    static GUID g_get_guid() {
        return guid_dsp_super_eq;
    }

    static void g_get_name(pfc::string_base& p_out) {
        p_out = "Super EQ (L/R Parametric & FIR)";
    }

    static bool g_get_default_preset(dsp_preset& p_out) {
        LiveSessionBus::state_to_preset(super_eq::EQPresetState::create_default_10band(), p_out);
        return true;
    }

    static bool g_have_config_popup() {
        return true;
    }

    static void g_show_config_popup(const dsp_preset& p_data, HWND p_parent, dsp_preset_edit_callback& p_callback) {
        super_eq::EQPresetState st = super_eq::EQPresetState::create_default_10band();
        LiveSessionBus::preset_to_state(p_data, st);
        SuperEqPopupDialog dlg(st, p_callback);
        dlg.run_modal(p_parent);
    }

    void on_endoftrack(abort_callback&) override {}

    void on_endofplayback(abort_callback&) override {
        engine_.reset();
    }

    bool on_chunk(audio_chunk* chunk, abort_callback&) override {
        if (!chunk) return true;
        t_size samples = chunk->get_sample_count();
        unsigned channels = chunk->get_channels();
        unsigned sr = chunk->get_sample_rate();
        if (samples == 0 || channels == 0 || sr == 0) return true;
        last_sample_rate_ = sr;

        // Upmix mono -> stereo when Left & Right processing differ
        if (channels == 1 && engine_.should_upmix_mono_to_stereo()) {
            const audio_sample* src = chunk->get_data();
            stereo_tmp_.resize(samples * 2);
            for (t_size i = 0; i < samples; ++i) {
                float s = static_cast<float>(src[i]);
                stereo_tmp_[i * 2] = s;
                stereo_tmp_[i * 2 + 1] = s;
            }
            engine_.process_interleaved(stereo_tmp_.data(), samples, 2, sr);
            if constexpr (sizeof(audio_sample) == sizeof(float)) {
                chunk->set_data(reinterpret_cast<const audio_sample*>(stereo_tmp_.data()),
                                samples, 2, sr, audio_chunk::channel_config_stereo);
            } else {
                sample_conv_.resize(samples * 2);
                for (size_t i = 0; i < samples * 2; ++i) sample_conv_[i] = stereo_tmp_[i];
                chunk->set_data(sample_conv_.data(), samples, 2, sr, audio_chunk::channel_config_stereo);
            }
            return true;
        }

        if constexpr (sizeof(audio_sample) == sizeof(float)) {
            engine_.process_interleaved(reinterpret_cast<float*>(chunk->get_data()), samples, channels, sr);
        } else {
            size_t total = samples * channels;
            float_tmp_.resize(total);
            audio_sample* ptr = chunk->get_data();
            for (size_t i = 0; i < total; ++i) float_tmp_[i] = static_cast<float>(ptr[i]);
            engine_.process_interleaved(float_tmp_.data(), samples, channels, sr);
            for (size_t i = 0; i < total; ++i) ptr[i] = static_cast<audio_sample>(float_tmp_[i]);
        }
        return true;
    }

    void flush() override {
        engine_.reset();
    }

    double get_latency() override {
        if (last_sample_rate_ == 0) return 0.0;
        return static_cast<double>(engine_.latency_samples()) / static_cast<double>(last_sample_rate_);
    }

    bool need_track_change_mark() override {
        return false;
    }

private:
    super_eq::SuperEQEngine engine_;
    unsigned last_sample_rate_ = 44100;
    std::vector<float> stereo_tmp_;
    std::vector<float> float_tmp_;
    std::vector<audio_sample> sample_conv_;
};

static dsp_factory_t<dsp_super_eq> g_dsp_super_eq_factory;

} // namespace super_eq_fb2k
