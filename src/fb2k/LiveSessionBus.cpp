#include "LiveSessionBus.h"
#include <algorithm>

namespace super_eq_fb2k {

static cfg_string cfg_saved_slot_a(guid_cfg_default_preset, "");
static cfg_string cfg_saved_slot_b(guid_cfg_ab_slot_b, "");

LiveSessionBus& LiveSessionBus::instance() {
    static LiveSessionBus inst;
    return inst;
}

LiveSessionBus::LiveSessionBus() {
    pfc::string8 s_a = cfg_saved_slot_a.get();
    if (s_a.get_length() > 0) {
        super_eq::EQPresetState st;
        if (super_eq::PresetIO::from_json(s_a.c_str(), st)) {
            ab_manager_.slot(0) = st;
        }
    }
    pfc::string8 s_b = cfg_saved_slot_b.get();
    if (s_b.get_length() > 0) {
        super_eq::EQPresetState st;
        if (super_eq::PresetIO::from_json(s_b.c_str(), st)) {
            ab_manager_.slot(1) = st;
        }
    }
}

void LiveSessionBus::register_engine(super_eq::SuperEQEngine* engine,
                                     const super_eq::EQPresetState& initial_state) {
    std::vector<ILiveViewListener*> targets;
    int slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_engine_ = engine;
        ab_manager_.current() = initial_state;
        slot = ab_manager_.active_slot();
        targets = listeners_;
    }
    notify_listeners(initial_state, slot, nullptr);
}

void LiveSessionBus::unregister_engine(super_eq::SuperEQEngine* engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_engine_ == engine) {
        active_engine_ = nullptr;
    }
}

void LiveSessionBus::add_listener(ILiveViewListener* listener) {
    if (!listener) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end()) {
        listeners_.push_back(listener);
    }
}

void LiveSessionBus::remove_listener(ILiveViewListener* listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

super_eq::EQPresetState LiveSessionBus::current_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ab_manager_.current();
}

int LiveSessionBus::active_ab_slot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ab_manager_.active_slot();
}

void LiveSessionBus::apply_user_state(const super_eq::EQPresetState& state,
                                      ILiveViewListener* source_listener,
                                      dsp_preset_edit_callback* popup_callback,
                                      bool live_only) {
    super_eq::EQPresetState clamped = state;
    clamped.clamp();
    int slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ab_manager_.current() = clamped;
        slot = ab_manager_.active_slot();
        if (active_engine_) {
            active_engine_->set_state(clamped);
        }
    }

    if (!live_only) {
        std::string json = super_eq::PresetIO::to_json(clamped);
        if (slot == 0) {
            cfg_saved_slot_a = json.c_str();
        } else {
            cfg_saved_slot_b = json.c_str();
        }

        if (popup_callback) {
            dsp_preset_impl p;
            state_to_preset(clamped, p);
            popup_callback->on_preset_changed(p);
        } else {
            commit_to_active_dsp_chain(clamped);
        }
    }

    notify_listeners(clamped, slot, source_listener);
}

void LiveSessionBus::switch_ab_slot(ILiveViewListener* source_listener,
                                    dsp_preset_edit_callback* popup_callback) {
    super_eq::EQPresetState st;
    int slot = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slot = ab_manager_.switch_slot();
        st = ab_manager_.current();
        if (active_engine_) {
            active_engine_->set_state(st);
        }
    }

    if (popup_callback) {
        dsp_preset_impl p;
        state_to_preset(st, p);
        popup_callback->on_preset_changed(p);
    } else {
        commit_to_active_dsp_chain(st);
    }

    notify_listeners(st, slot, source_listener);
}

void LiveSessionBus::copy_ab_to_other() {
    std::lock_guard<std::mutex> lock(mutex_);
    ab_manager_.copy_current_to_other();
    std::string json = super_eq::PresetIO::to_json(ab_manager_.current());
    if (ab_manager_.active_slot() == 0) {
        cfg_saved_slot_b = json.c_str();
    } else {
        cfg_saved_slot_a = json.c_str();
    }
}

bool LiveSessionBus::consume_spectrum_samples(super_eq::SpectrumAnalyzer& analyzer) {
    super_eq::SuperEQEngine* eng = nullptr;
    double sr = 44100.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        eng = active_engine_;
        if (eng) {
            sr = eng->sample_rate();
        }
    }
    if (!eng) return false;
    return analyzer.update_from_ring(eng->ring_buffer(), sr);
}

super_eq::MeterSnapshot LiveSessionBus::get_meters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_engine_) {
        return active_engine_->get_meters();
    }
    return super_eq::MeterSnapshot{};
}

void LiveSessionBus::reset_clip_latches() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_engine_) {
        active_engine_->reset_clip_latches();
    }
}

bool LiveSessionBus::is_dsp_active_in_chain() const {
    try {
        static_api_ptr_t<dsp_config_manager> mgr;
        dsp_chain_config_impl cfg;
        mgr->get_core_settings(cfg);
        for (t_size i = 0; i < cfg.get_count(); ++i) {
            if (cfg.get_item(i).get_owner() == guid_dsp_super_eq) {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

void LiveSessionBus::commit_to_active_dsp_chain(const super_eq::EQPresetState& st) {
    auto apply_fn = [st]() {
        try {
            static_api_ptr_t<dsp_config_manager> mgr;
            dsp_chain_config_impl cfg;
            mgr->get_core_settings(cfg);
            dsp_preset_impl preset;
            state_to_preset(st, preset);
            bool found = false;
            for (t_size i = 0; i < cfg.get_count(); ++i) {
                if (cfg.get_item(i).get_owner() == guid_dsp_super_eq) {
                    cfg.replace_item(preset, i);
                    found = true;
                }
            }
            if (found) {
                mgr->set_core_settings(cfg);
            }
        } catch (...) {
        }
    };

    if (core_api::is_main_thread()) {
        apply_fn();
    } else {
        fb2k::inMainThread(apply_fn);
    }
}

void LiveSessionBus::notify_listeners(const super_eq::EQPresetState& st,
                                      int slot,
                                      ILiveViewListener* exclude) {
    std::vector<ILiveViewListener*> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets = listeners_;
    }
    for (auto* l : targets) {
        if (l && l != exclude) {
            l->on_live_state_updated(st, slot);
        }
    }
}

void LiveSessionBus::state_to_preset(const super_eq::EQPresetState& st, dsp_preset& out) {
    out.set_owner(guid_dsp_super_eq);
    std::vector<uint8_t> blob = st.serialize_binary();
    out.set_data(blob.data(), blob.size());
}

bool LiveSessionBus::preset_to_state(const dsp_preset& p, super_eq::EQPresetState& out) {
    if (p.get_owner() != guid_dsp_super_eq) return false;
    return super_eq::EQPresetState::deserialize_binary(
        static_cast<const uint8_t*>(p.get_data()), p.get_data_size(), out);
}

} // namespace super_eq_fb2k
