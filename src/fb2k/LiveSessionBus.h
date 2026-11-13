#pragma once

#include "Guids.h"
#include "super_eq/dsp_engine.h"
#include "super_eq/preset_io.h"
#include <foobar2000/SDK/foobar2000.h>
#include <mutex>
#include <vector>

namespace super_eq_fb2k {

class ILiveViewListener {
public:
    virtual ~ILiveViewListener() = default;
    virtual void on_live_state_updated(const super_eq::EQPresetState& state, int active_ab_slot) = 0;
};

class LiveSessionBus {
public:
    static LiveSessionBus& instance();

    // Called by active playback dsp_super_eq instance
    void register_engine(super_eq::SuperEQEngine* engine, const super_eq::EQPresetState& initial_state);
    void unregister_engine(super_eq::SuperEQEngine* engine);

    // Observer registration for UI views (DSP Popup, UI Element, Modeless Window)
    void add_listener(ILiveViewListener* listener);
    void remove_listener(ILiveViewListener* listener);

    // State access & live mutation
    super_eq::EQPresetState current_state() const;
    int active_ab_slot() const;

    // Called when UI edits parameters. If popup_callback is non-null, notifies the DSP Manager popup callback;
    // otherwise commits directly into foobar2000's active DSP chain configuration (unless live_only is true during drag).
    void apply_user_state(const super_eq::EQPresetState& state,
                          ILiveViewListener* source_listener,
                          dsp_edit_callback_v2* popup_callback = nullptr,
                          bool live_only = false);

    void switch_ab_slot(ILiveViewListener* source_listener,
                        dsp_edit_callback_v2* popup_callback = nullptr);
    void copy_ab_to_other();

    // Audio telemetry from active playback engine
    bool consume_spectrum_samples(super_eq::SpectrumAnalyzer& analyzer);
    super_eq::MeterSnapshot get_meters() const;
    void reset_clip_latches();
    bool is_dsp_active_in_chain() const;

    // Preset conversion helpers
    static void state_to_preset(const super_eq::EQPresetState& st, dsp_preset& out);
    static bool preset_to_state(const dsp_preset& p, super_eq::EQPresetState& out);

private:
    LiveSessionBus();
    void commit_to_active_dsp_chain(const super_eq::EQPresetState& st);
    void notify_listeners(const super_eq::EQPresetState& st, int slot, ILiveViewListener* exclude);

    mutable std::mutex mutex_;
    super_eq::SuperEQEngine* active_engine_ = nullptr;
    super_eq::ABStateManager ab_manager_;
    std::vector<ILiveViewListener*> listeners_;
};

} // namespace super_eq_fb2k
