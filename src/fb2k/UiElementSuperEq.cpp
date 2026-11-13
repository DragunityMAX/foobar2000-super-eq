#include "Guids.h"
#include "LiveSessionBus.h"
#include "SuperEqEditorView.h"
#include <foobar2000/SDK/foobar2000.h>
#include <helpers/foobar2000+atl.h>

namespace super_eq_fb2k {

// -----------------------------------------------------------------------------
// 1. Default UI Dockable Element (ui_element_v2)
// -----------------------------------------------------------------------------
class ui_element_instance_super_eq
    : public ui_element_instance,
      public ATL::CWindowImpl<ui_element_instance_super_eq> {
public:
    DECLARE_WND_CLASS_EX(_T("SuperEqUiElementHost"), 0, COLOR_BTNFACE)

    ui_element_instance_super_eq(ui_element_config::ptr cfg, ui_element_instance_callback_ptr cb)
        : config_(cfg), callback_(cb) {}

    void initialize_window(HWND parent) {
        RECT rc{0, 0, 640, 360};
        Create(parent, rc, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    }

    HWND get_wnd() override { return m_hWnd; }

    void set_configuration(ui_element_config::ptr data) override { config_ = data; }
    ui_element_config::ptr get_configuration() override { return config_; }

    GUID get_guid() override { return guid_ui_element_super_eq; }
    GUID get_subclass() override { return ui_element_subclass_playback_visualisation; }

    void notify(const GUID& p_what, t_size, const void*, t_size) override {
        if (p_what == ui_element_notify_colors_changed || p_what == ui_element_notify_font_changed) {
            if (view_.m_hWnd) {
                view_.SendMessage(WM_SYSCOLORCHANGE, 0, 0);
            }
        }
    }

    BEGIN_MSG_MAP(ui_element_instance_super_eq)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
    END_MSG_MAP()

private:
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
        RECT rc{};
        GetClientRect(&rc);
        view_.set_initial_state(LiveSessionBus::instance().current_state());
        view_.Create(m_hWnd, rc, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN);
        return 0;
    }

    LRESULT OnSize(UINT, WPARAM, LPARAM lParam, BOOL&) {
        if (view_.m_hWnd) {
            view_.MoveWindow(0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    }

    ui_element_config::ptr config_;
    ui_element_instance_callback_ptr callback_;
    SuperEqEditorView view_;
};

class ui_element_super_eq : public ui_element_v2 {
public:
    GUID get_guid() override { return guid_ui_element_super_eq; }
    GUID get_subclass() override { return ui_element_subclass_playback_visualisation; }

    void get_name(pfc::string_base& out) override {
        out = "Super EQ (L/R Parametric & FIR)";
    }

    ui_element_instance_ptr instantiate(HWND parent,
                                        ui_element_config::ptr cfg,
                                        ui_element_instance_callback_ptr callback) override {
        service_ptr_t<ui_element_instance_super_eq> inst =
            new service_impl_t<ui_element_instance_super_eq>(cfg, callback);
        inst->initialize_window(parent);
        return inst;
    }

    ui_element_config::ptr get_default_configuration() override {
        return ui_element_config::g_create_empty(guid_ui_element_super_eq);
    }

    const char* get_description() override {
        return "Left/Right & Mid/Side separated up-to-64-band Parametric & Linear-Phase FIR Equalizer "
               "with real-time Pre/Post FFT spectrum analyzer.";
    }

    ui_element_children_enumerator_ptr enumerate_children(ui_element_config::ptr) override {
        return nullptr;
    }
};

static service_factory_single_t<ui_element_super_eq> g_ui_element_super_eq;

// -----------------------------------------------------------------------------
// 2. Modeless Floating Window (View -> Super EQ)
// -----------------------------------------------------------------------------
class SuperEqFloatingWindow : public ATL::CWindowImpl<SuperEqFloatingWindow> {
public:
    DECLARE_WND_CLASS_EX(_T("SuperEqFloatingWindow"), 0, COLOR_BTNFACE)

    static void show_or_focus() {
        static SuperEqFloatingWindow* s_inst = nullptr;
        if (s_inst && ::IsWindow(s_inst->m_hWnd)) {
            s_inst->ShowWindow(SW_RESTORE);
            s_inst->SetForegroundWindow(s_inst->m_hWnd);
            return;
        }
        if (!s_inst) {
            s_inst = new SuperEqFloatingWindow(&s_inst);
        }
        HWND main_wnd = core_api::get_main_window();
        RECT rc{0, 0, 1020, 640};
        s_inst->Create(main_wnd, rc, _T("Super EQ — Left/Right Parametric & Linear-Phase FIR"),
                       WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN, 0);
        s_inst->CenterWindow(main_wnd);
        s_inst->ShowWindow(SW_SHOW);
        s_inst->UpdateWindow();
    }

    BEGIN_MSG_MAP(SuperEqFloatingWindow)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

private:
    explicit SuperEqFloatingWindow(SuperEqFloatingWindow** self_ptr) : self_ptr_(self_ptr) {}

    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
        RECT rc{};
        GetClientRect(&rc);
        view_.set_initial_state(LiveSessionBus::instance().current_state());
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
        if (self_ptr_) *self_ptr_ = nullptr;
        return 0;
    }

    void OnFinalMessage(HWND) override {
        delete this;
    }

    SuperEqFloatingWindow** self_ptr_ = nullptr;
    SuperEqEditorView view_;
    fb2k::CCoreDarkModeHooks dark_mode_;
};

class mainmenu_commands_super_eq : public mainmenu_commands {
public:
    t_uint32 get_command_count() override { return 1; }

    GUID get_command(t_uint32 p_index) override {
        if (p_index == 0) return guid_mainmenu_cmd_open_super_eq;
        return pfc::guid_null;
    }

    void get_name(t_uint32 p_index, pfc::string_base& p_out) override {
        if (p_index == 0) p_out = "Super EQ (L/R Parametric & FIR)";
    }

    bool get_description(t_uint32 p_index, pfc::string_base& p_out) override {
        if (p_index == 0) {
            p_out = "Open the floating Super EQ Left/Right Parametric & Linear-Phase FIR Equalizer window.";
            return true;
        }
        return false;
    }

    GUID get_parent() override { return mainmenu_groups::view; }

    void execute(t_uint32 p_index, service_ptr_t<service_base>) override {
        if (p_index == 0) {
            SuperEqFloatingWindow::show_or_focus();
        }
    }
};

static mainmenu_commands_factory_t<mainmenu_commands_super_eq> g_mainmenu_super_eq;

} // namespace super_eq_fb2k
