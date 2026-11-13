#pragma once

#include <foobar2000/SDK/foobar2000.h>
#include <helpers/foobar2000+atl.h>
#include <helpers/DarkMode.h>

#include "LiveSessionBus.h"
#include "super_eq/dsp_engine.h"
#include "super_eq/preset_io.h"
#include "super_eq/spectrum_analyzer.h"

#include <d2d1.h>
#include <dwrite.h>
#include <commctrl.h>
#include <commdlg.h>
#include <vector>

namespace super_eq_fb2k {

enum class ViewChannelFilter {
    All = 0,
    Left = 1,
    Right = 2,
    Mid = 3,
    Side = 4
};

class SuperEqEditorView
    : public ATL::CWindowImpl<SuperEqEditorView>,
      public ILiveViewListener {
public:
    DECLARE_WND_CLASS_EX(_T("SuperEqEditorViewWnd"), CS_DBLCLKS, COLOR_WINDOW)

    SuperEqEditorView() = default;
    ~SuperEqEditorView() override;

    void set_popup_callback(dsp_preset_edit_callback* cb) { popup_callback_ = cb; }
    void set_initial_state(const super_eq::EQPresetState& st);
    const super_eq::EQPresetState& state() const { return state_; }

    void on_live_state_updated(const super_eq::EQPresetState& state, int active_ab_slot) override;

    BEGIN_MSG_MAP(SuperEqEditorView)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnLButtonDblClk)
        MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_COMMAND, OnCommand)
        MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
        MESSAGE_HANDLER(WM_HSCROLL, OnScroll)
        MESSAGE_HANDLER(WM_VSCROLL, OnScroll)
        MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnSysColorChange)
    END_MSG_MAP()

private:
    enum ControlIDs {
        IDC_BTN_BYPASS = 1001,
        IDC_BTN_AB,
        IDC_BTN_AB_COPY,
        IDC_COMBO_MODE,
        IDC_BTN_SPLIT,
        IDC_COMBO_CH_FILTER,
        IDC_CHK_LINK_LR,
        IDC_BTN_COPY_L2R,
        IDC_BTN_COPY_R2L,
        IDC_BTN_SWAP_LR,
        IDC_CHK_AUTOGAIN,
        IDC_CHK_LIMITER,
        IDC_CHK_SPECTRUM,
        IDC_CHK_PHASE,
        IDC_COMBO_DB_SCALE,
        IDC_BTN_TABLE_TOGGLE,
        IDC_BTN_PRESET_MENU,

        // Calibration right panel
        IDC_EDIT_GLOBAL_PREAMP,
        IDC_EDIT_L_PREAMP,
        IDC_EDIT_R_PREAMP,
        IDC_EDIT_L_DELAY,
        IDC_EDIT_R_DELAY,
        IDC_CHK_L_PHASE,
        IDC_CHK_R_PHASE,
        IDC_CHK_L_MUTE,
        IDC_CHK_R_MUTE,
        IDC_BTN_CAL_APPLY,

        // Band table & selected band bar
        IDC_LIST_BANDS,
        IDC_BTN_BAND_ADD,
        IDC_BTN_BAND_DEL,
        IDC_BTN_BAND_RESET,
        IDC_COMBO_SEL_ROUTE,
        IDC_COMBO_SEL_TYPE,
        IDC_COMBO_SEL_SLOPE,
        IDC_EDIT_SEL_FREQ,
        IDC_EDIT_SEL_GAIN,
        IDC_EDIT_SEL_Q,
        IDC_CHK_SEL_ON,
        IDC_CHK_SEL_SOLO,
        IDC_BTN_SEL_APPLY
    };

    static constexpr UINT_PTR kRenderTimerId = 1;
    static constexpr int kTopBarHeight = 62;
    static constexpr int kCalPanelWidth = 178;
    static constexpr int kMeterStripWidth = 54;
    static constexpr int kTableHeight = 190;

    // Lifecycle & layout
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&) { return 1; }
    LRESULT OnSysColorChange(UINT, WPARAM, LPARAM, BOOL&);

    // Mouse interaction on Direct2D canvas
    LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonDblClk(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnRButtonUp(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseWheel(UINT, WPARAM, LPARAM, BOOL&);

    // Child control handlers
    LRESULT OnCommand(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnNotify(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnScroll(UINT, WPARAM, LPARAM, BOOL&);

    void create_controls();
    void layout_controls();
    void sync_controls_from_state();
    void sync_band_table();
    void sync_selected_band_controls();
    void push_state_change(bool live_only = false);

    // Direct2D setup & rendering
    bool ensure_d2d_resources();
    void discard_d2d_resources();
    void render_canvas();
    void draw_graph_pane(const D2D1_RECT_F& rect,
                         super_eq::ChannelRoute pane_route,
                         bool draw_all_channels,
                         const wchar_t* label);
    void draw_meters_strip(const D2D1_RECT_F& rect);
    void draw_node_hud(const D2D1_RECT_F& graph_rect, int band_idx);

    // Coordinate transforms
    D2D1_RECT_F canvas_rect() const;
    D2D1_RECT_F graph_area_rect() const;
    D2D1_RECT_F meter_area_rect() const;
    void get_pane_rects(D2D1_RECT_F& top_rect, D2D1_RECT_F& bottom_rect) const;
    float freq_to_x(double freq_hz, const D2D1_RECT_F& r) const;
    double x_to_freq(float x, const D2D1_RECT_F& r) const;
    float db_to_y(double db, const D2D1_RECT_F& r) const;
    double y_to_db(float y, const D2D1_RECT_F& r) const;

    int hit_test_band(float x, float y, bool& out_bottom_pane) const;
    bool band_visible_in_pane(const super_eq::BandParams& b, bool is_split, bool bottom_pane) const;
    int find_linked_counterpart(int band_idx) const;

    // File import/export dialogs
    void show_preset_menu();
    void handle_import_apo(super_eq::ChannelRoute target_override, bool append);
    void handle_import_feq();
    void handle_export_apo();
    void handle_load_json();
    void handle_save_json();
    void show_band_context_menu(int band_idx, POINT screen_pt);

    void apply_dark_mode();

    // State
    super_eq::EQPresetState state_;
    int active_ab_slot_ = 0;
    dsp_preset_edit_callback* popup_callback_ = nullptr;
    super_eq::SpectrumAnalyzer analyzer_;
    super_eq::MeterSnapshot last_meters_{};

    // View options
    bool split_lr_view_ = false;
    ViewChannelFilter ch_filter_ = ViewChannelFilter::All;
    double db_range_ = 18.0;
    bool show_spectrum_ = true;
    bool show_phase_ = false;
    bool show_table_ = false;
    bool is_dark_mode_ = true;
    bool updating_controls_ = false;

    // Interaction state
    int selected_band_ = -1;
    int hovered_band_ = -1;
    int dragging_band_ = -1;
    bool drag_bottom_pane_ = false;
    POINT last_mouse_pt_{};

    // Direct2D / DirectWrite
    ID2D1Factory* d2d_factory_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    IDWriteFactory* dw_factory_ = nullptr;
    IDWriteTextFormat* tf_small_ = nullptr;
    IDWriteTextFormat* tf_hud_ = nullptr;
    IDWriteTextFormat* tf_header_ = nullptr;

    // Dark mode helper
    fb2k::CCoreDarkModeHooks dark_mode_;

    // Child controls
    HFONT ui_font_ = nullptr;
    HWND btn_bypass_ = nullptr;
    HWND btn_ab_ = nullptr;
    HWND btn_ab_copy_ = nullptr;
    HWND combo_mode_ = nullptr;
    HWND btn_split_ = nullptr;
    HWND combo_ch_filter_ = nullptr;
    HWND chk_link_lr_ = nullptr;
    HWND btn_copy_l2r_ = nullptr;
    HWND btn_copy_r2l_ = nullptr;
    HWND btn_swap_lr_ = nullptr;
    HWND chk_autogain_ = nullptr;
    HWND chk_limiter_ = nullptr;
    HWND chk_spectrum_ = nullptr;
    HWND chk_phase_ = nullptr;
    HWND combo_db_scale_ = nullptr;
    HWND btn_table_toggle_ = nullptr;
    HWND btn_preset_menu_ = nullptr;

    // Calibration controls
    HWND grp_cal_ = nullptr;
    HWND lbl_global_pre_ = nullptr, edit_global_pre_ = nullptr;
    HWND lbl_l_pre_ = nullptr, edit_l_pre_ = nullptr;
    HWND lbl_r_pre_ = nullptr, edit_r_pre_ = nullptr;
    HWND lbl_l_del_ = nullptr, edit_l_del_ = nullptr;
    HWND lbl_r_del_ = nullptr, edit_r_del_ = nullptr;
    HWND chk_l_phase_ = nullptr, chk_r_phase_ = nullptr;
    HWND chk_l_mute_ = nullptr, chk_r_mute_ = nullptr;
    HWND btn_cal_apply_ = nullptr;

    // Bottom table + selected band controls
    HWND list_bands_ = nullptr;
    HWND btn_band_add_ = nullptr;
    HWND btn_band_del_ = nullptr;
    HWND btn_band_reset_ = nullptr;
    HWND combo_sel_route_ = nullptr;
    HWND combo_sel_type_ = nullptr;
    HWND combo_sel_slope_ = nullptr;
    HWND edit_sel_freq_ = nullptr;
    HWND edit_sel_gain_ = nullptr;
    HWND edit_sel_q_ = nullptr;
    HWND chk_sel_on_ = nullptr;
    HWND chk_sel_solo_ = nullptr;
    HWND btn_sel_apply_ = nullptr;
};

} // namespace super_eq_fb2k
