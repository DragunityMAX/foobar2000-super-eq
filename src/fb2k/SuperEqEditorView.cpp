#include "SuperEqEditorView.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comctl32.lib")

namespace super_eq_fb2k {

namespace {

template <class T>
void safe_release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

D2D1_COLOR_F route_color(super_eq::ChannelRoute r, float alpha = 1.0f) {
    switch (r) {
        case super_eq::ChannelRoute::Left:   return D2D1::ColorF(0.00f, 0.83f, 1.00f, alpha); // Cyan #00D4FF
        case super_eq::ChannelRoute::Right:  return D2D1::ColorF(1.00f, 0.42f, 0.29f, alpha); // Coral #FF6B4A
        case super_eq::ChannelRoute::Mid:    return D2D1::ColorF(0.22f, 0.90f, 0.30f, alpha); // Green #38E54D
        case super_eq::ChannelRoute::Side:   return D2D1::ColorF(0.73f, 0.51f, 1.00f, alpha); // Purple #B983FF
        case super_eq::ChannelRoute::Stereo: return D2D1::ColorF(0.95f, 0.82f, 0.28f, alpha); // Gold #F2D147
    }
    return D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha);
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], len);
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], len, nullptr, nullptr);
    return s;
}

std::string read_file_utf8(const wchar_t* path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool write_file_utf8(const wchar_t* path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

double read_edit_double(HWND hwnd, double fallback) {
    wchar_t buf[64] = {};
    ::GetWindowTextW(hwnd, buf, 63);
    wchar_t* end = nullptr;
    double v = wcstod(buf, &end);
    if (end == buf || !std::isfinite(v)) return fallback;
    return v;
}

void set_edit_double(HWND hwnd, double val, int decimals) {
    wchar_t fmt[16];
    swprintf_s(fmt, L"%%.%df", decimals);
    wchar_t buf[64];
    swprintf_s(buf, fmt, val);
    ::SetWindowTextW(hwnd, buf);
}

} // namespace

SuperEqEditorView::~SuperEqEditorView() {
    discard_d2d_resources();
    safe_release(tf_small_);
    safe_release(tf_hud_);
    safe_release(tf_header_);
    safe_release(dw_factory_);
    safe_release(d2d_factory_);
}

void SuperEqEditorView::set_initial_state(const super_eq::EQPresetState& st) {
    state_ = st;
    state_.clamp();
    if (m_hWnd) {
        sync_controls_from_state();
        Invalidate(FALSE);
    }
}

void SuperEqEditorView::on_live_state_updated(const super_eq::EQPresetState& state, int active_ab_slot) {
    auto update_fn = [this, state, active_ab_slot]() {
        if (!::IsWindow(m_hWnd)) return;
        state_ = state;
        active_ab_slot_ = active_ab_slot;
        sync_controls_from_state();
        Invalidate(FALSE);
    };
    if (core_api::is_main_thread()) {
        update_fn();
    } else {
        fb2k::inMainThread(update_fn);
    }
}

LRESULT SuperEqEditorView::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
    state_ = LiveSessionBus::instance().current_state();
    active_ab_slot_ = LiveSessionBus::instance().active_ab_slot();

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&dw_factory_));
    if (dw_factory_) {
        dw_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      10.5f, L"en-us", &tf_small_);
        dw_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      11.5f, L"en-us", &tf_hud_);
        dw_factory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      12.0f, L"en-us", &tf_header_);
    }

    ui_font_ = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    create_controls();
    apply_dark_mode();
    layout_controls();
    sync_controls_from_state();

    LiveSessionBus::instance().add_listener(this);
    SetTimer(kRenderTimerId, 16, nullptr); // ~60 fps
    return 0;
}

LRESULT SuperEqEditorView::OnDestroy(UINT, WPARAM, LPARAM, BOOL&) {
    KillTimer(kRenderTimerId);
    LiveSessionBus::instance().remove_listener(this);
    discard_d2d_resources();
    return 0;
}

LRESULT SuperEqEditorView::OnSize(UINT, WPARAM, LPARAM lParam, BOOL&) {
    UINT w = LOWORD(lParam);
    UINT h = HIWORD(lParam);
    if (rt_ && w > 0 && h > 0) {
        rt_->Resize(D2D1::SizeU(w, h));
    }
    layout_controls();
    Invalidate(FALSE);
    return 0;
}

LRESULT SuperEqEditorView::OnSysColorChange(UINT, WPARAM, LPARAM, BOOL&) {
    apply_dark_mode();
    Invalidate(FALSE);
    return 0;
}

void SuperEqEditorView::apply_dark_mode() {
    dark_mode_.AddDialogWithControls(m_hWnd);
    is_dark_mode_ = fb2k::isDarkMode();
}

LRESULT SuperEqEditorView::OnTimer(UINT, WPARAM wParam, LPARAM, BOOL&) {
    if (wParam == kRenderTimerId) {
        if (show_spectrum_) {
            LiveSessionBus::instance().consume_spectrum_samples(analyzer_);
        }
        last_meters_ = LiveSessionBus::instance().get_meters();
        render_canvas();
    }
    return 0;
}

LRESULT SuperEqEditorView::OnPaint(UINT, WPARAM, LPARAM, BOOL&) {
    PAINTSTRUCT ps;
    BeginPaint(&ps);
    render_canvas();
    EndPaint(&ps);
    return 0;
}

void SuperEqEditorView::create_controls() {
    auto mk_btn = [&](const wchar_t* text, int id, DWORD extra_style = 0) -> HWND {
        HWND h = ::CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extra_style,
                                   0, 0, 60, 22, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr, nullptr);
        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        return h;
    };
    auto mk_chk = [&](const wchar_t* text, int id) -> HWND {
        HWND h = ::CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   0, 0, 70, 20, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr, nullptr);
        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        return h;
    };
    auto mk_combo = [&](int id) -> HWND {
        HWND h = ::CreateWindowExW(0, L"COMBOBOX", nullptr,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   0, 0, 90, 200, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr, nullptr);
        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        return h;
    };
    auto mk_lbl = [&](const wchar_t* text) -> HWND {
        HWND h = ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                   0, 0, 80, 16, m_hWnd, nullptr, nullptr, nullptr);
        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        return h;
    };
    auto mk_edit = [&](int id) -> HWND {
        HWND h = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.0",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   0, 0, 56, 20, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   nullptr, nullptr);
        ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
        return h;
    };

    // Top bar row 1 & 2
    btn_preset_menu_ = mk_btn(L"Presets / I/O ▾", IDC_BTN_PRESET_MENU);
    btn_bypass_ = mk_btn(L"Bypass: OFF", IDC_BTN_BYPASS);
    btn_ab_ = mk_btn(L"Slot: A", IDC_BTN_AB);
    btn_ab_copy_ = mk_btn(L"A→B", IDC_BTN_AB_COPY);

    combo_mode_ = mk_combo(IDC_COMBO_MODE);
    ::SendMessageW(combo_mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Min-Phase IIR"));
    ::SendMessageW(combo_mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Linear-Phase FIR"));

    btn_split_ = mk_btn(L"View: Overlay", IDC_BTN_SPLIT);

    combo_ch_filter_ = mk_combo(IDC_COMBO_CH_FILTER);
    ::SendMessageW(combo_ch_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All Channels"));
    ::SendMessageW(combo_ch_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Left (L)"));
    ::SendMessageW(combo_ch_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Right (R)"));
    ::SendMessageW(combo_ch_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Mid (M)"));
    ::SendMessageW(combo_ch_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Side (S)"));

    chk_link_lr_ = mk_chk(L"Link L+R", IDC_CHK_LINK_LR);
    btn_copy_l2r_ = mk_btn(L"L→R", IDC_BTN_COPY_L2R);
    btn_copy_r2l_ = mk_btn(L"R→L", IDC_BTN_COPY_R2L);
    btn_swap_lr_ = mk_btn(L"L↔R", IDC_BTN_SWAP_LR);

    chk_autogain_ = mk_chk(L"Auto-Gain", IDC_CHK_AUTOGAIN);
    chk_limiter_ = mk_chk(L"Limiter", IDC_CHK_LIMITER);
    chk_spectrum_ = mk_chk(L"Spectrum", IDC_CHK_SPECTRUM);
    chk_phase_ = mk_chk(L"Phase", IDC_CHK_PHASE);

    combo_db_scale_ = mk_combo(IDC_COMBO_DB_SCALE);
    ::SendMessageW(combo_db_scale_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"±3 dB"));
    ::SendMessageW(combo_db_scale_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"±6 dB"));
    ::SendMessageW(combo_db_scale_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"±12 dB"));
    ::SendMessageW(combo_db_scale_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"±18 dB"));
    ::SendMessageW(combo_db_scale_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"±30 dB"));
    ::SendMessageW(combo_db_scale_, CB_SETCURSEL, 3, 0);

    btn_table_toggle_ = mk_btn(L"Band Table ▾", IDC_BTN_TABLE_TOGGLE);

    // Right Calibration Panel
    grp_cal_ = ::CreateWindowExW(0, L"BUTTON", L"L/R Calibration",
                                 WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                 0, 0, 160, 260, m_hWnd, nullptr, nullptr, nullptr);
    ::SendMessageW(grp_cal_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);

    lbl_global_pre_ = mk_lbl(L"Global Preamp (dB):");
    edit_global_pre_ = mk_edit(IDC_EDIT_GLOBAL_PREAMP);
    lbl_l_pre_ = mk_lbl(L"Left Preamp (dB):");
    edit_l_pre_ = mk_edit(IDC_EDIT_L_PREAMP);
    lbl_r_pre_ = mk_lbl(L"Right Preamp (dB):");
    edit_r_pre_ = mk_edit(IDC_EDIT_R_PREAMP);
    lbl_l_del_ = mk_lbl(L"Left Delay (ms):");
    edit_l_del_ = mk_edit(IDC_EDIT_L_DELAY);
    lbl_r_del_ = mk_lbl(L"Right Delay (ms):");
    edit_r_del_ = mk_edit(IDC_EDIT_R_DELAY);
    chk_l_phase_ = mk_chk(L"Ø Invert L", IDC_CHK_L_PHASE);
    chk_r_phase_ = mk_chk(L"Ø Invert R", IDC_CHK_R_PHASE);
    chk_l_mute_ = mk_chk(L"Mute L", IDC_CHK_L_MUTE);
    chk_r_mute_ = mk_chk(L"Mute R", IDC_CHK_R_MUTE);
    btn_cal_apply_ = mk_btn(L"Apply Cal", IDC_BTN_CAL_APPLY);

    // Bottom Collapsible Band Table + Selected Band Bar
    list_bands_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                    WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                    0, 0, 400, 140, m_hWnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST_BANDS)),
                                    nullptr, nullptr);
    ::SendMessageW(list_bands_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    ListView_SetExtendedListViewStyle(list_bands_, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    const wchar_t* cols[] = {L"#", L"On", L"Solo", L"Channel", L"Type", L"Freq (Hz)", L"Gain (dB)", L"Q", L"Slope"};
    const int widths[] = {32, 36, 40, 68, 86, 82, 74, 62, 78};
    for (int i = 0; i < 9; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(cols[i]);
        col.cx = widths[i];
        col.iSubItem = i;
        ListView_InsertColumn(list_bands_, i, &col);
    }

    btn_band_add_ = mk_btn(L"+ Band", IDC_BTN_BAND_ADD);
    btn_band_del_ = mk_btn(L"- Delete", IDC_BTN_BAND_DEL);
    btn_band_reset_ = mk_btn(L"Reset Flat", IDC_BTN_BAND_RESET);

    combo_sel_route_ = mk_combo(IDC_COMBO_SEL_ROUTE);
    for (int i = 0; i <= 4; ++i) {
        auto w = utf8_to_wide(super_eq::channel_route_name(static_cast<super_eq::ChannelRoute>(i)));
        ::SendMessageW(combo_sel_route_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
    combo_sel_type_ = mk_combo(IDC_COMBO_SEL_TYPE);
    for (int i = 0; i <= static_cast<int>(super_eq::FilterType::FlatTilt); ++i) {
        auto w = utf8_to_wide(super_eq::filter_type_name(static_cast<super_eq::FilterType>(i)));
        ::SendMessageW(combo_sel_type_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }
    combo_sel_slope_ = mk_combo(IDC_COMBO_SEL_SLOPE);
    const super_eq::FilterSlope slopes[] = {
        super_eq::FilterSlope::dB6, super_eq::FilterSlope::dB12, super_eq::FilterSlope::dB18,
        super_eq::FilterSlope::dB24, super_eq::FilterSlope::dB36, super_eq::FilterSlope::dB48
    };
    for (auto s : slopes) {
        auto w = utf8_to_wide(super_eq::filter_slope_name(s));
        ::SendMessageW(combo_sel_slope_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w.c_str()));
    }

    edit_sel_freq_ = mk_edit(IDC_EDIT_SEL_FREQ);
    edit_sel_gain_ = mk_edit(IDC_EDIT_SEL_GAIN);
    edit_sel_q_ = mk_edit(IDC_EDIT_SEL_Q);
    chk_sel_on_ = mk_chk(L"On", IDC_CHK_SEL_ON);
    chk_sel_solo_ = mk_chk(L"Solo", IDC_CHK_SEL_SOLO);
    btn_sel_apply_ = mk_btn(L"Apply", IDC_BTN_SEL_APPLY);
}

void SuperEqEditorView::layout_controls() {
    if (!m_hWnd) return;
    RECT rc{};
    GetClientRect(&rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    // Row 1 (y = 6)
    int x = 6, y = 6;
    auto place = [&](HWND h, int w, int hgt = 23) {
        ::MoveWindow(h, x, y, w, hgt, TRUE);
        x += w + 4;
    };
    place(btn_preset_menu_, 100);
    place(btn_bypass_, 82);
    place(btn_ab_, 58);
    place(btn_ab_copy_, 40);
    place(combo_mode_, 114, 140);
    place(btn_split_, 94);
    place(combo_ch_filter_, 96, 160);
    place(combo_db_scale_, 68, 160);

    // Row 2 (y = 33)
    x = 6;
    y = 33;
    place(chk_link_lr_, 70, 22);
    place(btn_copy_l2r_, 42, 22);
    place(btn_copy_r2l_, 42, 22);
    place(btn_swap_lr_, 42, 22);
    place(chk_autogain_, 76, 22);
    place(chk_limiter_, 64, 22);
    place(chk_spectrum_, 74, 22);
    place(chk_phase_, 58, 22);
    place(btn_table_toggle_, 90, 22);

    // Right Calibration Panel
    int cal_x = std::max(300, W - kCalPanelWidth);
    int cal_y = kTopBarHeight + 4;
    int cal_h = std::max(260, H - cal_y - (show_table_ ? kTableHeight : 0) - 6);
    ::MoveWindow(grp_cal_, cal_x, cal_y, kCalPanelWidth - 6, cal_h, TRUE);

    int cx = cal_x + 10;
    int cy = cal_y + 18;
    int cw = kCalPanelWidth - 26;
    auto cal_row = [&](HWND lbl, HWND ed) {
        ::MoveWindow(lbl, cx, cy, cw - 62, 18, TRUE);
        ::MoveWindow(ed, cx + cw - 58, cy - 2, 58, 20, TRUE);
        cy += 24;
    };
    cal_row(lbl_global_pre_, edit_global_pre_);
    cal_row(lbl_l_pre_, edit_l_pre_);
    cal_row(lbl_r_pre_, edit_r_pre_);
    cal_row(lbl_l_del_, edit_l_del_);
    cal_row(lbl_r_del_, edit_r_del_);
    ::MoveWindow(chk_l_phase_, cx, cy, 80, 20, TRUE);
    ::MoveWindow(chk_r_phase_, cx + 80, cy, 78, 20, TRUE);
    cy += 22;
    ::MoveWindow(chk_l_mute_, cx, cy, 80, 20, TRUE);
    ::MoveWindow(chk_r_mute_, cx + 80, cy, 78, 20, TRUE);
    cy += 26;
    ::MoveWindow(btn_cal_apply_, cx, cy, cw, 24, TRUE);

    // Collapsible Bottom Band Table
    int show_flag = show_table_ ? SW_SHOW : SW_HIDE;
    HWND table_ctrls[] = {
        list_bands_, btn_band_add_, btn_band_del_, btn_band_reset_,
        combo_sel_route_, combo_sel_type_, combo_sel_slope_,
        edit_sel_freq_, edit_sel_gain_, edit_sel_q_,
        chk_sel_on_, chk_sel_solo_, btn_sel_apply_
    };
    for (HWND h : table_ctrls) {
        ::ShowWindow(h, show_flag);
    }

    if (show_table_) {
        int ty = H - kTableHeight;
        int bx = 6;
        ::MoveWindow(btn_band_add_, bx, ty + 2, 62, 22, TRUE); bx += 66;
        ::MoveWindow(btn_band_del_, bx, ty + 2, 62, 22, TRUE); bx += 66;
        ::MoveWindow(btn_band_reset_, bx, ty + 2, 74, 22, TRUE); bx += 80;
        ::MoveWindow(chk_sel_on_, bx, ty + 2, 42, 22, TRUE); bx += 44;
        ::MoveWindow(chk_sel_solo_, bx, ty + 2, 48, 22, TRUE); bx += 50;
        ::MoveWindow(combo_sel_route_, bx, ty + 2, 68, 160, TRUE); bx += 72;
        ::MoveWindow(combo_sel_type_, bx, ty + 2, 82, 200, TRUE); bx += 86;
        ::MoveWindow(combo_sel_slope_, bx, ty + 2, 78, 160, TRUE); bx += 82;
        ::MoveWindow(edit_sel_freq_, bx, ty + 2, 60, 22, TRUE); bx += 64;
        ::MoveWindow(edit_sel_gain_, bx, ty + 2, 52, 22, TRUE); bx += 56;
        ::MoveWindow(edit_sel_q_, bx, ty + 2, 48, 22, TRUE); bx += 52;
        ::MoveWindow(btn_sel_apply_, bx, ty + 2, 50, 22, TRUE);

        ::MoveWindow(list_bands_, 6, ty + 28, std::max(200, W - 12), kTableHeight - 34, TRUE);
    }
}

void SuperEqEditorView::sync_controls_from_state() {
    if (!m_hWnd) return;
    updating_controls_ = true;

    ::SetWindowTextW(btn_bypass_, state_.global_bypass ? L"Bypass: ON" : L"Bypass: OFF");
    ::SetWindowTextW(btn_ab_, (active_ab_slot_ == 0) ? L"Slot: A" : L"Slot: B");
    ::SetWindowTextW(btn_ab_copy_, (active_ab_slot_ == 0) ? L"A→B" : L"B→A");
    ::SendMessageW(combo_mode_, CB_SETCURSEL,
                   state_.processing_mode == super_eq::ProcessingMode::LinearPhaseFIR ? 1 : 0, 0);
    ::SetWindowTextW(btn_split_, split_lr_view_ ? L"View: Split L/R" : L"View: Overlay");
    ::SendMessageW(combo_ch_filter_, CB_SETCURSEL, static_cast<int>(ch_filter_), 0);

    ::SendMessageW(chk_link_lr_, BM_SETCHECK, state_.link_lr ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chk_autogain_, BM_SETCHECK, state_.auto_gain ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chk_limiter_, BM_SETCHECK, state_.clip_limiter ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chk_spectrum_, BM_SETCHECK, show_spectrum_ ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chk_phase_, BM_SETCHECK, show_phase_ ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SetWindowTextW(btn_table_toggle_, show_table_ ? L"Band Table ▴" : L"Band Table ▾");

    set_edit_double(edit_global_pre_, state_.global_preamp_db, 2);
    set_edit_double(edit_l_pre_, state_.left_cal.preamp_db, 2);
    set_edit_double(edit_r_pre_, state_.right_cal.preamp_db, 2);
    set_edit_double(edit_l_del_, state_.left_cal.delay_ms, 2);
    set_edit_double(edit_r_del_, state_.right_cal.delay_ms, 2);

    ::SendMessageW(chk_l_phase_, BM_SETCHECK, state_.left_cal.invert_phase ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chk_r_phase_, BM_SETCHECK, state_.right_cal.invert_phase ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chk_l_mute_, BM_SETCHECK, state_.left_cal.mute ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chk_r_mute_, BM_SETCHECK, state_.right_cal.mute ? BST_CHECKED : BST_UNCHECKED, 0);

    if (selected_band_ >= static_cast<int>(state_.bands.size())) {
        selected_band_ = static_cast<int>(state_.bands.size()) - 1;
    }

    sync_band_table();
    sync_selected_band_controls();
    updating_controls_ = false;
}

void SuperEqEditorView::sync_band_table() {
    if (!list_bands_) return;
    ListView_DeleteAllItems(list_bands_);
    for (int i = 0; i < static_cast<int>(state_.bands.size()); ++i) {
        const auto& b = state_.bands[static_cast<size_t>(i)];
        wchar_t buf[64];
        swprintf_s(buf, L"%d", i + 1);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = buf;
        ListView_InsertItem(list_bands_, &item);

        ListView_SetItemText(list_bands_, i, 1, const_cast<LPWSTR>(b.enabled ? L"ON" : L"--"));
        ListView_SetItemText(list_bands_, i, 2, const_cast<LPWSTR>(b.solo ? L"S" : L""));
        std::wstring wroute = utf8_to_wide(super_eq::channel_route_name(b.route));
        ListView_SetItemText(list_bands_, i, 3, const_cast<LPWSTR>(wroute.c_str()));
        std::wstring wtype = utf8_to_wide(super_eq::filter_type_name(b.type));
        ListView_SetItemText(list_bands_, i, 4, const_cast<LPWSTR>(wtype.c_str()));

        swprintf_s(buf, L"%.1f", b.freq_hz);
        ListView_SetItemText(list_bands_, i, 5, buf);
        swprintf_s(buf, L"%+.2f", b.gain_db);
        ListView_SetItemText(list_bands_, i, 6, buf);
        swprintf_s(buf, L"%.2f", b.q);
        ListView_SetItemText(list_bands_, i, 7, buf);
        std::wstring wslope = utf8_to_wide(super_eq::filter_slope_name(b.slope));
        ListView_SetItemText(list_bands_, i, 8, const_cast<LPWSTR>(wslope.c_str()));
    }
    if (selected_band_ >= 0 && selected_band_ < static_cast<int>(state_.bands.size())) {
        ListView_SetItemState(list_bands_, selected_band_, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

void SuperEqEditorView::sync_selected_band_controls() {
    bool has = (selected_band_ >= 0 && selected_band_ < static_cast<int>(state_.bands.size()));
    HWND ctrls[] = {combo_sel_route_, combo_sel_type_, combo_sel_slope_,
                    edit_sel_freq_, edit_sel_gain_, edit_sel_q_,
                    chk_sel_on_, chk_sel_solo_, btn_sel_apply_, btn_band_del_};
    for (HWND h : ctrls) ::EnableWindow(h, has ? TRUE : FALSE);
    if (!has) return;

    const auto& b = state_.bands[static_cast<size_t>(selected_band_)];
    ::SendMessageW(chk_sel_on_, BM_SETCHECK, b.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(chk_sel_solo_, BM_SETCHECK, b.solo ? BST_CHECKED : BST_UNCHECKED, 0);
    ::SendMessageW(combo_sel_route_, CB_SETCURSEL, static_cast<int>(b.route), 0);
    ::SendMessageW(combo_sel_type_, CB_SETCURSEL, static_cast<int>(b.type), 0);
    int slope_idx = 1;
    switch (b.slope) {
        case super_eq::FilterSlope::dB6: slope_idx = 0; break;
        case super_eq::FilterSlope::dB12: slope_idx = 1; break;
        case super_eq::FilterSlope::dB18: slope_idx = 2; break;
        case super_eq::FilterSlope::dB24: slope_idx = 3; break;
        case super_eq::FilterSlope::dB36: slope_idx = 4; break;
        case super_eq::FilterSlope::dB48: slope_idx = 5; break;
    }
    ::SendMessageW(combo_sel_slope_, CB_SETCURSEL, slope_idx, 0);
    set_edit_double(edit_sel_freq_, b.freq_hz, 1);
    set_edit_double(edit_sel_gain_, b.gain_db, 2);
    set_edit_double(edit_sel_q_, b.q, 2);
}

void SuperEqEditorView::push_state_change(bool live_only) {
    state_.clamp();
    LiveSessionBus::instance().apply_user_state(state_, this, popup_callback_, live_only);
    if (!live_only) {
        sync_band_table();
        sync_selected_band_controls();
    }
    Invalidate(FALSE);
}

D2D1_RECT_F SuperEqEditorView::canvas_rect() const {
    RECT rc{};
    GetClientRect(&rc);
    float right = static_cast<float>(std::max(100, (rc.right - rc.left) - kCalPanelWidth - 4));
    float bottom = static_cast<float>(std::max(120, (rc.bottom - rc.top) - (show_table_ ? kTableHeight : 0) - 4));
    return D2D1::RectF(6.0f, static_cast<float>(kTopBarHeight), right, bottom);
}

D2D1_RECT_F SuperEqEditorView::graph_area_rect() const {
    D2D1_RECT_F c = canvas_rect();
    c.right = std::max(c.left + 80.0f, c.right - static_cast<float>(kMeterStripWidth) - 4.0f);
    return c;
}

D2D1_RECT_F SuperEqEditorView::meter_area_rect() const {
    D2D1_RECT_F c = canvas_rect();
    D2D1_RECT_F g = graph_area_rect();
    return D2D1::RectF(g.right + 4.0f, c.top, c.right, c.bottom);
}

void SuperEqEditorView::get_pane_rects(D2D1_RECT_F& top_rect, D2D1_RECT_F& bottom_rect) const {
    D2D1_RECT_F g = graph_area_rect();
    if (!split_lr_view_) {
        top_rect = g;
        bottom_rect = D2D1::RectF(0, 0, 0, 0);
    } else {
        float mid_y = 0.5f * (g.top + g.bottom);
        top_rect = D2D1::RectF(g.left, g.top, g.right, mid_y - 3.0f);
        bottom_rect = D2D1::RectF(g.left, mid_y + 3.0f, g.right, g.bottom);
    }
}

float SuperEqEditorView::freq_to_x(double freq_hz, const D2D1_RECT_F& r) const {
    double f = super_eq::clamp_double(freq_hz, 20.0, 20000.0);
    double t = (std::log10(f) - std::log10(20.0)) / (std::log10(20000.0) - std::log10(20.0));
    return r.left + static_cast<float>(t) * (r.right - r.left);
}

double SuperEqEditorView::x_to_freq(float x, const D2D1_RECT_F& r) const {
    double w = std::max(1.0f, r.right - r.left);
    double t = super_eq::clamp_double((x - r.left) / w, 0.0, 1.0);
    return 20.0 * std::pow(1000.0, t);
}

float SuperEqEditorView::db_to_y(double db, const D2D1_RECT_F& r) const {
    double clamped = super_eq::clamp_double(db, -db_range_, db_range_);
    double t = (db_range_ - clamped) / (2.0 * db_range_);
    return r.top + static_cast<float>(t) * (r.bottom - r.top);
}

double SuperEqEditorView::y_to_db(float y, const D2D1_RECT_F& r) const {
    double h = std::max(1.0f, r.bottom - r.top);
    double t = super_eq::clamp_double((y - r.top) / h, 0.0, 1.0);
    return db_range_ - t * (2.0 * db_range_);
}

bool SuperEqEditorView::band_visible_in_pane(const super_eq::BandParams& b,
                                              bool is_split,
                                              bool bottom_pane) const {
    if (is_split) {
        if (!bottom_pane) {
            return b.route == super_eq::ChannelRoute::Left || b.route == super_eq::ChannelRoute::Stereo;
        }
        return b.route == super_eq::ChannelRoute::Right || b.route == super_eq::ChannelRoute::Stereo;
    }
    switch (ch_filter_) {
        case ViewChannelFilter::All:   return true;
        case ViewChannelFilter::Left:  return b.route == super_eq::ChannelRoute::Left || b.route == super_eq::ChannelRoute::Stereo;
        case ViewChannelFilter::Right: return b.route == super_eq::ChannelRoute::Right || b.route == super_eq::ChannelRoute::Stereo;
        case ViewChannelFilter::Mid:   return b.route == super_eq::ChannelRoute::Mid;
        case ViewChannelFilter::Side:  return b.route == super_eq::ChannelRoute::Side;
    }
    return true;
}

int SuperEqEditorView::find_linked_counterpart(int band_idx) const {
    if (band_idx < 0 || band_idx >= static_cast<int>(state_.bands.size())) return -1;
    const auto& src = state_.bands[static_cast<size_t>(band_idx)];
    if (src.route != super_eq::ChannelRoute::Left && src.route != super_eq::ChannelRoute::Right) {
        return -1;
    }
    super_eq::ChannelRoute target = (src.route == super_eq::ChannelRoute::Left)
                                        ? super_eq::ChannelRoute::Right
                                        : super_eq::ChannelRoute::Left;
    int best = -1;
    double best_score = 1e9;
    for (int i = 0; i < static_cast<int>(state_.bands.size()); ++i) {
        if (i == band_idx) continue;
        const auto& c = state_.bands[static_cast<size_t>(i)];
        if (c.route == target && c.type == src.type) {
            double score = std::abs(std::log2(c.freq_hz / src.freq_hz));
            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
    }
    return (best_score < 0.35) ? best : -1;
}

int SuperEqEditorView::hit_test_band(float x, float y, bool& out_bottom_pane) const {
    D2D1_RECT_F top_r{}, bot_r{};
    get_pane_rects(top_r, bot_r);
    out_bottom_pane = false;
    D2D1_RECT_F active_r = top_r;
    if (split_lr_view_ && y >= bot_r.top) {
        out_bottom_pane = true;
        active_r = bot_r;
    }

    int best = -1;
    float best_d2 = 144.0f; // 12 px radius
    for (int i = 0; i < static_cast<int>(state_.bands.size()); ++i) {
        const auto& b = state_.bands[static_cast<size_t>(i)];
        if (!band_visible_in_pane(b, split_lr_view_, out_bottom_pane)) continue;
        float nx = freq_to_x(b.freq_hz, active_r);
        float ny = db_to_y(b.gain_db, active_r);
        float dx = x - nx;
        float dy = y - ny;
        float d2 = dx * dx + dy * dy;
        if (d2 <= best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

bool SuperEqEditorView::ensure_d2d_resources() {
    if (!d2d_factory_ || !m_hWnd) return false;
    if (rt_) return true;
    RECT rc{};
    GetClientRect(&rc);
    D2D1_SIZE_U sz = D2D1::SizeU(
        static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
        static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));
    HRESULT hr = d2d_factory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(m_hWnd, sz),
        &rt_);
    return SUCCEEDED(hr) && rt_ != nullptr;
}

void SuperEqEditorView::discard_d2d_resources() {
    safe_release(rt_);
}

void SuperEqEditorView::render_canvas() {
    if (!ensure_d2d_resources()) return;
    D2D1_RECT_F c = canvas_rect();
    if (c.right <= c.left || c.bottom <= c.top) return;

    rt_->BeginDraw();
    rt_->PushAxisAlignedClip(c, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    D2D1_COLOR_F bg = is_dark_mode_ ? D2D1::ColorF(0.08f, 0.09f, 0.11f, 1.0f)
                                    : D2D1::ColorF(0.96f, 0.97f, 0.98f, 1.0f);
    ID2D1SolidColorBrush* br_bg = nullptr;
    rt_->CreateSolidColorBrush(bg, &br_bg);
    if (br_bg) {
        rt_->FillRectangle(c, br_bg);
        br_bg->Release();
    }

    D2D1_RECT_F top_r{}, bot_r{};
    get_pane_rects(top_r, bot_r);

    if (!split_lr_view_) {
        draw_graph_pane(top_r, super_eq::ChannelRoute::Stereo, true, L"Composite L / R / M / S");
    } else {
        draw_graph_pane(top_r, super_eq::ChannelRoute::Left, false, L"LEFT CHANNEL (L + Stereo)");
        draw_graph_pane(bot_r, super_eq::ChannelRoute::Right, false, L"RIGHT CHANNEL (R + Stereo)");
    }

    draw_meters_strip(meter_area_rect());

    // Draw floating HUD for hovered or selected band
    int hud_idx = (hovered_band_ >= 0) ? hovered_band_ : selected_band_;
    if (hud_idx >= 0 && hud_idx < static_cast<int>(state_.bands.size())) {
        D2D1_RECT_F hr = top_r;
        if (split_lr_view_ && state_.bands[static_cast<size_t>(hud_idx)].route == super_eq::ChannelRoute::Right) {
            hr = bot_r;
        }
        draw_node_hud(hr, hud_idx);
    }

    rt_->PopAxisAlignedClip();
    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_d2d_resources();
    }
}

void SuperEqEditorView::draw_graph_pane(const D2D1_RECT_F& r,
                                        super_eq::ChannelRoute pane_route,
                                        bool draw_all_channels,
                                        const wchar_t* label) {
    if (r.right <= r.left + 20.0f || r.bottom <= r.top + 20.0f) return;

    ID2D1SolidColorBrush* br_grid = nullptr;
    ID2D1SolidColorBrush* br_zero = nullptr;
    ID2D1SolidColorBrush* br_text = nullptr;
    rt_->CreateSolidColorBrush(is_dark_mode_ ? D2D1::ColorF(0.20f, 0.23f, 0.28f, 0.85f)
                                             : D2D1::ColorF(0.80f, 0.82f, 0.86f, 0.90f), &br_grid);
    rt_->CreateSolidColorBrush(is_dark_mode_ ? D2D1::ColorF(0.42f, 0.46f, 0.54f, 0.95f)
                                             : D2D1::ColorF(0.45f, 0.48f, 0.54f, 0.95f), &br_zero);
    rt_->CreateSolidColorBrush(is_dark_mode_ ? D2D1::ColorF(0.75f, 0.78f, 0.84f, 0.90f)
                                             : D2D1::ColorF(0.20f, 0.22f, 0.26f, 0.90f), &br_text);

    // Horizontal dB grid
    double step = (db_range_ <= 6.0) ? 1.0 : ((db_range_ <= 12.0) ? 3.0 : 6.0);
    for (double g = -db_range_; g <= db_range_ + 0.001; g += step) {
        float y = db_to_y(g, r);
        bool is_zero = std::abs(g) < 1e-4;
        rt_->DrawLine(D2D1::Point2F(r.left, y), D2D1::Point2F(r.right, y),
                      is_zero ? br_zero : br_grid, is_zero ? 1.2f : 0.75f);
        if (tf_small_ && br_text) {
            wchar_t lbl[16];
            swprintf_s(lbl, L"%+.0f", g);
            rt_->DrawTextW(lbl, static_cast<UINT32>(wcslen(lbl)), tf_small_,
                           D2D1::RectF(r.left + 3.0f, y - 13.0f, r.left + 40.0f, y + 2.0f), br_text);
        }
    }

    // Vertical frequency grid
    const double grid_freqs[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    const wchar_t* freq_lbls[] = {L"20", L"50", L"100", L"200", L"500", L"1k", L"2k", L"5k", L"10k", L"20k"};
    for (int i = 0; i < 10; ++i) {
        float x = freq_to_x(grid_freqs[i], r);
        rt_->DrawLine(D2D1::Point2F(x, r.top), D2D1::Point2F(x, r.bottom), br_grid, 0.75f);
        if (tf_small_ && br_text) {
            rt_->DrawTextW(freq_lbls[i], static_cast<UINT32>(wcslen(freq_lbls[i])), tf_small_,
                           D2D1::RectF(x + 2.0f, r.bottom - 15.0f, x + 36.0f, r.bottom), br_text);
        }
    }

    // Border & label
    rt_->DrawRectangle(r, br_grid, 1.0f);
    if (tf_header_ && br_text && label) {
        rt_->DrawTextW(label, static_cast<UINT32>(wcslen(label)), tf_header_,
                       D2D1::RectF(r.left + 36.0f, r.top + 4.0f, r.left + 280.0f, r.top + 22.0f), br_text);
    }

    // Real-time Pre/Post FFT Spectrum
    if (show_spectrum_) {
        auto draw_spec = [&](const std::vector<float>& db_pts, D2D1_COLOR_F col) {
            const auto& freqs = analyzer_.freqs();
            if (freqs.size() < 2 || db_pts.size() != freqs.size()) return;
            ID2D1PathGeometry* geo = nullptr;
            if (FAILED(d2d_factory_->CreatePathGeometry(&geo))) return;
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(geo->Open(&sink))) {
                auto map_spec_y = [&](float db) -> float {
                    double t = super_eq::clamp_double((0.0 - db) / 90.0, 0.0, 1.0);
                    return r.top + static_cast<float>(t) * (r.bottom - r.top);
                };
                sink->BeginFigure(D2D1::Point2F(freq_to_x(freqs[0], r), r.bottom), D2D1_FIGURE_BEGIN_FILLED);
                for (size_t i = 0; i < freqs.size(); ++i) {
                    sink->AddLine(D2D1::Point2F(freq_to_x(freqs[i], r), map_spec_y(db_pts[i])));
                }
                sink->AddLine(D2D1::Point2F(freq_to_x(freqs.back(), r), r.bottom));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();

                ID2D1SolidColorBrush* br = nullptr;
                rt_->CreateSolidColorBrush(col, &br);
                if (br) {
                    rt_->FillGeometry(geo, br);
                    br->Release();
                }
            }
            geo->Release();
        };

        if (draw_all_channels || pane_route == super_eq::ChannelRoute::Left) {
            draw_spec(analyzer_.pre_left_db(), D2D1::ColorF(0.45f, 0.55f, 0.65f, 0.14f));
            draw_spec(analyzer_.post_left_db(), D2D1::ColorF(0.00f, 0.83f, 1.00f, 0.18f));
        }
        if (draw_all_channels || pane_route == super_eq::ChannelRoute::Right) {
            draw_spec(analyzer_.pre_right_db(), D2D1::ColorF(0.60f, 0.45f, 0.45f, 0.12f));
            draw_spec(analyzer_.post_right_db(), D2D1::ColorF(1.00f, 0.42f, 0.29f, 0.16f));
        }
    }

    // Selected/Hovered band shaded fill
    int focus_band = (hovered_band_ >= 0) ? hovered_band_ : selected_band_;
    if (focus_band >= 0 && focus_band < static_cast<int>(state_.bands.size())) {
        const auto& b = state_.bands[static_cast<size_t>(focus_band)];
        if (band_visible_in_pane(b, !draw_all_channels, pane_route == super_eq::ChannelRoute::Right)) {
            ID2D1PathGeometry* geo = nullptr;
            if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&geo))) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(geo->Open(&sink))) {
                    float zero_y = db_to_y(0.0, r);
                    sink->BeginFigure(D2D1::Point2F(r.left, zero_y), D2D1_FIGURE_BEGIN_FILLED);
                    const int steps = 160;
                    for (int i = 0; i <= steps; ++i) {
                        double t = static_cast<double>(i) / steps;
                        double f = 20.0 * std::pow(1000.0, t);
                        double mag = super_eq::SuperEQEngine::eval_single_band_magnitude_db(b, f, 44100.0);
                        sink->AddLine(D2D1::Point2F(freq_to_x(f, r), db_to_y(mag, r)));
                    }
                    sink->AddLine(D2D1::Point2F(r.right, zero_y));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    sink->Release();

                    ID2D1SolidColorBrush* br_fill = nullptr;
                    rt_->CreateSolidColorBrush(route_color(b.route, 0.18f), &br_fill);
                    if (br_fill) {
                        rt_->FillGeometry(geo, br_fill);
                        br_fill->Release();
                    }
                }
                geo->Release();
            }
        }
    }

    // Composite response curves
    auto draw_curve = [&](super_eq::ChannelRoute target, D2D1_COLOR_F col, float width, bool phase) {
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(d2d_factory_->CreatePathGeometry(&geo))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(geo->Open(&sink))) {
            const int steps = 240;
            for (int i = 0; i <= steps; ++i) {
                double t = static_cast<double>(i) / steps;
                double f = 20.0 * std::pow(1000.0, t);
                double val = phase
                    ? (super_eq::SuperEQEngine::eval_channel_phase_deg(state_, target, f, 44100.0) / 180.0) * db_range_
                    : super_eq::SuperEQEngine::eval_channel_magnitude_db(state_, target, f, 44100.0, false);
                D2D1_POINT_2F pt = D2D1::Point2F(freq_to_x(f, r), db_to_y(val, r));
                if (i == 0) sink->BeginFigure(pt, D2D1_FIGURE_BEGIN_HOLLOW);
                else sink->AddLine(pt);
            }
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            sink->Release();

            ID2D1SolidColorBrush* br = nullptr;
            rt_->CreateSolidColorBrush(col, &br);
            if (br) {
                rt_->DrawGeometry(geo, br, width);
                br->Release();
            }
        }
        geo->Release();
    };

    if (draw_all_channels) {
        if (ch_filter_ == ViewChannelFilter::All || ch_filter_ == ViewChannelFilter::Left) {
            draw_curve(super_eq::ChannelRoute::Left, route_color(super_eq::ChannelRoute::Left, 0.95f), 2.2f, false);
            if (show_phase_) draw_curve(super_eq::ChannelRoute::Left, route_color(super_eq::ChannelRoute::Left, 0.40f), 1.0f, true);
        }
        if (ch_filter_ == ViewChannelFilter::All || ch_filter_ == ViewChannelFilter::Right) {
            draw_curve(super_eq::ChannelRoute::Right, route_color(super_eq::ChannelRoute::Right, 0.95f), 2.2f, false);
            if (show_phase_) draw_curve(super_eq::ChannelRoute::Right, route_color(super_eq::ChannelRoute::Right, 0.40f), 1.0f, true);
        }
        if (state_.has_ms_bands() || ch_filter_ == ViewChannelFilter::Mid) {
            draw_curve(super_eq::ChannelRoute::Mid, route_color(super_eq::ChannelRoute::Mid, 0.85f), 1.6f, false);
        }
        if (state_.has_ms_bands() || ch_filter_ == ViewChannelFilter::Side) {
            draw_curve(super_eq::ChannelRoute::Side, route_color(super_eq::ChannelRoute::Side, 0.85f), 1.6f, false);
        }
    } else {
        draw_curve(pane_route, route_color(pane_route, 0.95f), 2.2f, false);
        if (show_phase_) draw_curve(pane_route, route_color(pane_route, 0.45f), 1.0f, true);
    }

    // Band Nodes
    bool is_bottom = (!draw_all_channels && pane_route == super_eq::ChannelRoute::Right);
    for (int i = 0; i < static_cast<int>(state_.bands.size()); ++i) {
        const auto& b = state_.bands[static_cast<size_t>(i)];
        if (!band_visible_in_pane(b, !draw_all_channels, is_bottom)) continue;

        float nx = freq_to_x(b.freq_hz, r);
        float ny = db_to_y(b.gain_db, r);
        bool is_sel = (i == selected_band_ || i == hovered_band_);

        D2D1_COLOR_F col = route_color(b.route, b.enabled ? 0.95f : 0.35f);
        ID2D1SolidColorBrush* br_node = nullptr;
        rt_->CreateSolidColorBrush(col, &br_node);
        if (br_node) {
            D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(nx, ny), is_sel ? 8.0f : 6.5f, is_sel ? 8.0f : 6.5f);
            if (b.enabled) {
                rt_->FillEllipse(el, br_node);
            } else {
                rt_->DrawEllipse(el, br_node, 1.5f);
            }
            if (is_sel) {
                D2D1_ELLIPSE halo = D2D1::Ellipse(D2D1::Point2F(nx, ny), 12.0f, 12.0f);
                rt_->DrawEllipse(halo, br_node, 1.5f);
            }
            if (b.solo) {
                ID2D1SolidColorBrush* br_solo = nullptr;
                rt_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.90f, 0.20f, 1.0f), &br_solo);
                if (br_solo) {
                    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(nx, ny), 10.5f, 10.5f), br_solo, 2.0f);
                    br_solo->Release();
                }
            }
            br_node->Release();
        }

        if (tf_small_) {
            ID2D1SolidColorBrush* br_num = nullptr;
            rt_->CreateSolidColorBrush(b.enabled ? D2D1::ColorF(0.05f, 0.05f, 0.07f, 1.0f)
                                                 : D2D1::ColorF(0.85f, 0.85f, 0.85f, 0.8f), &br_num);
            if (br_num) {
                wchar_t nstr[8];
                swprintf_s(nstr, L"%d", i + 1);
                rt_->DrawTextW(nstr, static_cast<UINT32>(wcslen(nstr)), tf_small_,
                               D2D1::RectF(nx - 6.0f, ny - 7.0f, nx + 10.0f, ny + 8.0f), br_num);
                br_num->Release();
            }
        }
    }

    safe_release(br_grid);
    safe_release(br_zero);
    safe_release(br_text);
}

void SuperEqEditorView::draw_meters_strip(const D2D1_RECT_F& r) {
    if (r.right <= r.left + 8.0f || r.bottom <= r.top + 30.0f) return;

    ID2D1SolidColorBrush* br_bg = nullptr;
    ID2D1SolidColorBrush* br_l = nullptr;
    ID2D1SolidColorBrush* br_r = nullptr;
    ID2D1SolidColorBrush* br_clip = nullptr;
    ID2D1SolidColorBrush* br_txt = nullptr;

    rt_->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.06f, 0.08f, 1.0f), &br_bg);
    rt_->CreateSolidColorBrush(route_color(super_eq::ChannelRoute::Left, 0.9f), &br_l);
    rt_->CreateSolidColorBrush(route_color(super_eq::ChannelRoute::Right, 0.9f), &br_r);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.18f, 0.18f, 1.0f), &br_clip);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0.80f, 0.82f, 0.86f, 1.0f), &br_txt);

    rt_->FillRectangle(r, br_bg);

    // Top Clip Latch indicator (height 18 px)
    D2D1_RECT_F clip_l = D2D1::RectF(r.left + 4.0f, r.top + 4.0f, r.left + 23.0f, r.top + 18.0f);
    D2D1_RECT_F clip_r = D2D1::RectF(r.left + 27.0f, r.top + 4.0f, r.right - 4.0f, r.top + 18.0f);
    if (last_meters_.clip_l) rt_->FillRectangle(clip_l, br_clip);
    else rt_->DrawRectangle(clip_l, br_txt, 0.8f);
    if (last_meters_.clip_r) rt_->FillRectangle(clip_r, br_clip);
    else rt_->DrawRectangle(clip_r, br_txt, 0.8f);

    float bar_top = r.top + 24.0f;
    float bar_bot = r.bottom - 18.0f;
    float bar_h = std::max(10.0f, bar_bot - bar_top);

    auto amp_to_y = [&](float amp) -> float {
        double db = super_eq::clamp_double(super_eq::linear_to_db(std::max(1e-6f, amp)), -60.0, 6.0);
        double t = (6.0 - db) / 66.0;
        return bar_top + static_cast<float>(t) * bar_h;
    };

    float y_l_rms = amp_to_y(last_meters_.rms_l);
    float y_r_rms = amp_to_y(last_meters_.rms_r);
    float y_l_pk = amp_to_y(last_meters_.peak_l);
    float y_r_pk = amp_to_y(last_meters_.peak_r);

    rt_->FillRectangle(D2D1::RectF(r.left + 6.0f, y_l_rms, r.left + 21.0f, bar_bot), br_l);
    rt_->FillRectangle(D2D1::RectF(r.left + 28.0f, y_r_rms, r.right - 6.0f, bar_bot), br_r);

    rt_->DrawLine(D2D1::Point2F(r.left + 5.0f, y_l_pk), D2D1::Point2F(r.left + 22.0f, y_l_pk), br_txt, 2.0f);
    rt_->DrawLine(D2D1::Point2F(r.left + 27.0f, y_r_pk), D2D1::Point2F(r.right - 5.0f, y_r_pk), br_txt, 2.0f);

    if (tf_small_ && br_txt) {
        rt_->DrawTextW(L"L   R", 5, tf_small_,
                       D2D1::RectF(r.left + 8.0f, bar_bot + 2.0f, r.right - 2.0f, r.bottom), br_txt);
    }

    safe_release(br_bg);
    safe_release(br_l);
    safe_release(br_r);
    safe_release(br_clip);
    safe_release(br_txt);
}

void SuperEqEditorView::draw_node_hud(const D2D1_RECT_F& graph_rect, int band_idx) {
    const auto& b = state_.bands[static_cast<size_t>(band_idx)];
    float nx = freq_to_x(b.freq_hz, graph_rect);
    float ny = db_to_y(b.gain_db, graph_rect);

    const float hud_w = 196.0f;
    const float hud_h = 54.0f;
    float hx = nx + 14.0f;
    if (hx + hud_w > graph_rect.right - 6.0f) hx = nx - hud_w - 14.0f;
    float hy = ny - hud_h - 8.0f;
    if (hy < graph_rect.top + 4.0f) hy = ny + 14.0f;

    D2D1_RECT_F hr = D2D1::RectF(hx, hy, hx + hud_w, hy + hud_h);
    ID2D1SolidColorBrush* br_bg = nullptr;
    ID2D1SolidColorBrush* br_border = nullptr;
    ID2D1SolidColorBrush* br_txt = nullptr;

    rt_->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.07f, 0.09f, 0.90f), &br_bg);
    rt_->CreateSolidColorBrush(route_color(b.route, 0.95f), &br_border);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.96f, 0.98f, 1.0f), &br_txt);

    rt_->FillRectangle(hr, br_bg);
    rt_->DrawRectangle(hr, br_border, 1.3f);

    if (tf_hud_ && br_txt) {
        wchar_t line1[128], line2[128];
        std::wstring wroute = utf8_to_wide(super_eq::channel_route_name(b.route));
        std::wstring wtype = utf8_to_wide(super_eq::filter_type_name(b.type));
        swprintf_s(line1, L"#%d [%s] %s%s%s", band_idx + 1, wroute.c_str(), wtype.c_str(),
                   b.solo ? L" [SOLO]" : L"", b.enabled ? L"" : L" [OFF]");
        swprintf_s(line2, L"%.1f Hz | %+.2f dB | Q %.2f", b.freq_hz, b.gain_db, b.q);
        rt_->DrawTextW(line1, static_cast<UINT32>(wcslen(line1)), tf_hud_,
                       D2D1::RectF(hr.left + 6.0f, hr.top + 4.0f, hr.right - 4.0f, hr.top + 24.0f), br_border);
        rt_->DrawTextW(line2, static_cast<UINT32>(wcslen(line2)), tf_small_ ? tf_small_ : tf_hud_,
                       D2D1::RectF(hr.left + 6.0f, hr.top + 26.0f, hr.right - 4.0f, hr.bottom - 4.0f), br_txt);
    }

    safe_release(br_bg);
    safe_release(br_border);
    safe_release(br_txt);
}

LRESULT SuperEqEditorView::OnMouseMove(UINT, WPARAM wParam, LPARAM lParam, BOOL&) {
    float mx = static_cast<float>(static_cast<short>(LOWORD(lParam)));
    float my = static_cast<float>(static_cast<short>(HIWORD(lParam)));

    if (dragging_band_ >= 0 && dragging_band_ < static_cast<int>(state_.bands.size())) {
        D2D1_RECT_F top_r{}, bot_r{};
        get_pane_rects(top_r, bot_r);
        D2D1_RECT_F r = (split_lr_view_ && drag_bottom_pane_) ? bot_r : top_r;

        auto& b = state_.bands[static_cast<size_t>(dragging_band_)];
        bool fine = (wParam & MK_SHIFT) != 0;
        bool alt_q = (::GetKeyState(VK_MENU) & 0x8000) != 0;

        double old_f = b.freq_hz;
        double old_g = b.gain_db;
        double old_q = b.q;

        if (alt_q) {
            double dy = static_cast<double>(last_mouse_pt_.y - static_cast<int>(my));
            b.q = super_eq::clamp_double(b.q * std::pow(1.01, dy * (fine ? 0.2 : 1.0)), super_eq::kMinQ, super_eq::kMaxQ);
        } else {
            double target_f = x_to_freq(mx, r);
            double target_g = y_to_db(my, r);
            if (fine) {
                b.freq_hz = super_eq::clamp_double(old_f + (target_f - old_f) * 0.12, super_eq::kMinFreqHz, super_eq::kMaxFreqHz);
                b.gain_db = super_eq::clamp_double(old_g + (target_g - old_g) * 0.12, super_eq::kMinGainDb, super_eq::kMaxGainDb);
            } else {
                b.freq_hz = target_f;
                b.gain_db = target_g;
            }
        }
        b.clamp();

        if (state_.link_lr) {
            int other = find_linked_counterpart(dragging_band_);
            if (other >= 0) {
                auto& ob = state_.bands[static_cast<size_t>(other)];
                ob.freq_hz += (b.freq_hz - old_f);
                ob.gain_db += (b.gain_db - old_g);
                ob.q += (b.q - old_q);
                ob.clamp();
            }
        }

        last_mouse_pt_.x = static_cast<LONG>(mx);
        last_mouse_pt_.y = static_cast<LONG>(my);
        push_state_change(true);
        return 0;
    }

    bool dummy = false;
    int hit = hit_test_band(mx, my, dummy);
    if (hit != hovered_band_) {
        hovered_band_ = hit;
        render_canvas();
    }
    return 0;
}

LRESULT SuperEqEditorView::OnLButtonDown(UINT, WPARAM, LPARAM lParam, BOOL&) {
    float mx = static_cast<float>(static_cast<short>(LOWORD(lParam)));
    float my = static_cast<float>(static_cast<short>(HIWORD(lParam)));
    last_mouse_pt_.x = static_cast<LONG>(mx);
    last_mouse_pt_.y = static_cast<LONG>(my);

    D2D1_RECT_F mr = meter_area_rect();
    if (mx >= mr.left && mx <= mr.right && my >= mr.top && my <= mr.top + 22.0f) {
        LiveSessionBus::instance().reset_clip_latches();
        last_meters_.clip_l = false;
        last_meters_.clip_r = false;
        render_canvas();
        return 0;
    }

    bool bottom = false;
    int hit = hit_test_band(mx, my, bottom);
    if (hit >= 0) {
        selected_band_ = hit;
        dragging_band_ = hit;
        drag_bottom_pane_ = bottom;
        SetCapture();
        sync_selected_band_controls();
        render_canvas();
    }
    return 0;
}

LRESULT SuperEqEditorView::OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&) {
    if (dragging_band_ >= 0) {
        dragging_band_ = -1;
        ReleaseCapture();
        push_state_change(false);
    }
    return 0;
}

LRESULT SuperEqEditorView::OnLButtonDblClk(UINT, WPARAM, LPARAM lParam, BOOL&) {
    float mx = static_cast<float>(static_cast<short>(LOWORD(lParam)));
    float my = static_cast<float>(static_cast<short>(HIWORD(lParam)));
    D2D1_RECT_F g = graph_area_rect();
    if (mx < g.left || mx > g.right || my < g.top || my > g.bottom) return 0;

    bool bottom = false;
    int hit = hit_test_band(mx, my, bottom);
    if (hit >= 0) {
        state_.bands[static_cast<size_t>(hit)].enabled = !state_.bands[static_cast<size_t>(hit)].enabled;
        selected_band_ = hit;
        push_state_change(false);
        return 0;
    }

    if (state_.bands.size() >= super_eq::kMaxBands) return 0;
    D2D1_RECT_F top_r{}, bot_r{};
    get_pane_rects(top_r, bot_r);
    bool in_bot = (split_lr_view_ && my >= bot_r.top);
    D2D1_RECT_F r = in_bot ? bot_r : top_r;

    super_eq::BandParams nb;
    nb.enabled = true;
    nb.type = super_eq::FilterType::Bell;
    nb.slope = super_eq::FilterSlope::dB12;
    nb.freq_hz = x_to_freq(mx, r);
    nb.gain_db = y_to_db(my, r);
    nb.q = 1.41421356;

    if (split_lr_view_) {
        nb.route = in_bot ? super_eq::ChannelRoute::Right : super_eq::ChannelRoute::Left;
    } else {
        switch (ch_filter_) {
            case ViewChannelFilter::All:   nb.route = super_eq::ChannelRoute::Stereo; break;
            case ViewChannelFilter::Left:  nb.route = super_eq::ChannelRoute::Left; break;
            case ViewChannelFilter::Right: nb.route = super_eq::ChannelRoute::Right; break;
            case ViewChannelFilter::Mid:   nb.route = super_eq::ChannelRoute::Mid; break;
            case ViewChannelFilter::Side:  nb.route = super_eq::ChannelRoute::Side; break;
        }
    }
    nb.clamp();
    state_.bands.push_back(nb);
    selected_band_ = static_cast<int>(state_.bands.size()) - 1;
    push_state_change(false);
    return 0;
}

LRESULT SuperEqEditorView::OnMouseWheel(UINT, WPARAM wParam, LPARAM lParam, BOOL&) {
    POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
    ScreenToClient(&pt);
    bool dummy = false;
    int target = hit_test_band(static_cast<float>(pt.x), static_cast<float>(pt.y), dummy);
    if (target < 0) target = selected_band_;
    if (target >= 0 && target < static_cast<int>(state_.bands.size())) {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        double factor = (delta > 0) ? 1.12 : (1.0 / 1.12);
        auto& b = state_.bands[static_cast<size_t>(target)];
        b.q = super_eq::clamp_double(b.q * factor, super_eq::kMinQ, super_eq::kMaxQ);
        if (state_.link_lr) {
            int other = find_linked_counterpart(target);
            if (other >= 0) state_.bands[static_cast<size_t>(other)].q = b.q;
        }
        push_state_change(false);
    }
    return 0;
}

LRESULT SuperEqEditorView::OnRButtonUp(UINT, WPARAM, LPARAM lParam, BOOL&) {
    POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
    bool dummy = false;
    int hit = hit_test_band(static_cast<float>(pt.x), static_cast<float>(pt.y), dummy);
    if (hit >= 0) {
        selected_band_ = hit;
        sync_selected_band_controls();
        ClientToScreen(&pt);
        show_band_context_menu(hit, pt);
    }
    return 0;
}

void SuperEqEditorView::show_band_context_menu(int band_idx, POINT screen_pt) {
    if (band_idx < 0 || band_idx >= static_cast<int>(state_.bands.size())) return;
    auto& b = state_.bands[static_cast<size_t>(band_idx)];

    HMENU menu = ::CreatePopupMenu();
    HMENU sub_type = ::CreatePopupMenu();
    HMENU sub_slope = ::CreatePopupMenu();
    HMENU sub_route = ::CreatePopupMenu();

    for (int i = 0; i <= static_cast<int>(super_eq::FilterType::FlatTilt); ++i) {
        std::wstring w = utf8_to_wide(super_eq::filter_type_name(static_cast<super_eq::FilterType>(i)));
        UINT flags = MF_STRING | (static_cast<int>(b.type) == i ? MF_CHECKED : 0);
        ::AppendMenuW(sub_type, flags, 100 + i, w.c_str());
    }

    const super_eq::FilterSlope slopes[] = {
        super_eq::FilterSlope::dB6, super_eq::FilterSlope::dB12, super_eq::FilterSlope::dB18,
        super_eq::FilterSlope::dB24, super_eq::FilterSlope::dB36, super_eq::FilterSlope::dB48
    };
    for (int i = 0; i < 6; ++i) {
        std::wstring w = utf8_to_wide(super_eq::filter_slope_name(slopes[i]));
        UINT flags = MF_STRING | (b.slope == slopes[i] ? MF_CHECKED : 0);
        ::AppendMenuW(sub_slope, flags, 200 + i, w.c_str());
    }

    for (int i = 0; i <= 4; ++i) {
        std::wstring w = utf8_to_wide(super_eq::channel_route_name(static_cast<super_eq::ChannelRoute>(i)));
        UINT flags = MF_STRING | (static_cast<int>(b.route) == i ? MF_CHECKED : 0);
        ::AppendMenuW(sub_route, flags, 300 + i, w.c_str());
    }

    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub_type), L"Filter Type");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub_slope), L"Filter Slope");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub_route), L"Channel Route");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (b.enabled ? MF_CHECKED : 0), 401, L"Enabled");
    ::AppendMenuW(menu, MF_STRING | (b.solo ? MF_CHECKED : 0), 402, L"Solo Band");
    if (b.route == super_eq::ChannelRoute::Left || b.route == super_eq::ChannelRoute::Right) {
        ::AppendMenuW(menu, MF_STRING, 403, L"Duplicate to Opposite Channel");
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 404, L"Delete Band");

    int cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                               screen_pt.x, screen_pt.y, 0, m_hWnd, nullptr);
    ::DestroyMenu(menu);

    if (cmd >= 100 && cmd < 120) {
        b.type = static_cast<super_eq::FilterType>(cmd - 100);
        push_state_change(false);
    } else if (cmd >= 200 && cmd < 210) {
        b.slope = slopes[cmd - 200];
        push_state_change(false);
    } else if (cmd >= 300 && cmd < 310) {
        b.route = static_cast<super_eq::ChannelRoute>(cmd - 300);
        push_state_change(false);
    } else if (cmd == 401) {
        b.enabled = !b.enabled;
        push_state_change(false);
    } else if (cmd == 402) {
        b.solo = !b.solo;
        push_state_change(false);
    } else if (cmd == 403) {
        if (state_.bands.size() < super_eq::kMaxBands) {
            super_eq::BandParams copy = b;
            copy.route = (b.route == super_eq::ChannelRoute::Left)
                             ? super_eq::ChannelRoute::Right
                             : super_eq::ChannelRoute::Left;
            state_.bands.push_back(copy);
            push_state_change(false);
        }
    } else if (cmd == 404) {
        state_.bands.erase(state_.bands.begin() + band_idx);
        if (selected_band_ >= static_cast<int>(state_.bands.size())) {
            selected_band_ = static_cast<int>(state_.bands.size()) - 1;
        }
        hovered_band_ = -1;
        push_state_change(false);
    }
}

void SuperEqEditorView::show_preset_menu() {
    RECT r{};
    ::GetWindowRect(btn_preset_menu_, &r);
    HMENU menu = ::CreatePopupMenu();
    ::AppendMenuW(menu, MF_STRING, 1, L"Load JSON Preset...");
    ::AppendMenuW(menu, MF_STRING, 2, L"Save JSON Preset...");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 3, L"Import Equalizer APO / REW (.txt) [As in File]");
    ::AppendMenuW(menu, MF_STRING, 4, L"Import APO / REW (.txt) -> Force Left (Append)");
    ::AppendMenuW(menu, MF_STRING, 5, L"Import APO / REW (.txt) -> Force Right (Append)");
    ::AppendMenuW(menu, MF_STRING, 6, L"Import foobar2000 (.feq)...");
    ::AppendMenuW(menu, MF_STRING, 7, L"Export Equalizer APO (.txt)...");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, 8, L"Reset to Default 10-Band Flat");

    int cmd = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                               r.left, r.bottom, 0, m_hWnd, nullptr);
    ::DestroyMenu(menu);

    switch (cmd) {
        case 1: handle_load_json(); break;
        case 2: handle_save_json(); break;
        case 3: handle_import_apo(super_eq::ChannelRoute::Stereo, false); break;
        case 4: handle_import_apo(super_eq::ChannelRoute::Left, true); break;
        case 5: handle_import_apo(super_eq::ChannelRoute::Right, true); break;
        case 6: handle_import_feq(); break;
        case 7: handle_export_apo(); break;
        case 8:
            state_ = super_eq::EQPresetState::create_default_10band();
            selected_band_ = -1;
            push_state_change(false);
            break;
        default: break;
    }
}

void SuperEqEditorView::handle_import_apo(super_eq::ChannelRoute target_override, bool append) {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"Equalizer APO / REW (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!::GetOpenFileNameW(&ofn)) return;

    std::string txt = read_file_utf8(path);
    if (super_eq::PresetIO::import_apo_text(txt, state_, target_override, append)) {
        push_state_change(false);
        sync_controls_from_state();
    }
}

void SuperEqEditorView::handle_import_feq() {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"foobar2000 Equalizer (*.feq)\0*.feq\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!::GetOpenFileNameW(&ofn)) return;

    std::string txt = read_file_utf8(path);
    if (super_eq::PresetIO::import_feq_text(txt, state_, super_eq::ChannelRoute::Stereo)) {
        push_state_change(false);
        sync_controls_from_state();
    }
}

void SuperEqEditorView::handle_export_apo() {
    wchar_t path[MAX_PATH] = L"super_eq_apo.txt";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"Equalizer APO (*.txt)\0*.txt\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!::GetSaveFileNameW(&ofn)) return;

    write_file_utf8(path, super_eq::PresetIO::export_apo_text(state_));
}

void SuperEqEditorView::handle_load_json() {
    wchar_t path[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"Super EQ JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!::GetOpenFileNameW(&ofn)) return;

    super_eq::EQPresetState loaded;
    if (super_eq::PresetIO::from_json(read_file_utf8(path), loaded)) {
        state_ = std::move(loaded);
        push_state_change(false);
        sync_controls_from_state();
    }
}

void SuperEqEditorView::handle_save_json() {
    wchar_t path[MAX_PATH] = L"super_eq_preset.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"Super EQ JSON (*.json)\0*.json\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!::GetSaveFileNameW(&ofn)) return;

    write_file_utf8(path, super_eq::PresetIO::to_json(state_));
}

LRESULT SuperEqEditorView::OnCommand(UINT, WPARAM wParam, LPARAM, BOOL&) {
    if (updating_controls_) return 0;
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);

    switch (id) {
        case IDC_BTN_PRESET_MENU:
            show_preset_menu();
            break;
        case IDC_BTN_BYPASS:
            state_.global_bypass = !state_.global_bypass;
            ::SetWindowTextW(btn_bypass_, state_.global_bypass ? L"Bypass: ON" : L"Bypass: OFF");
            push_state_change(false);
            break;
        case IDC_BTN_AB:
            LiveSessionBus::instance().switch_ab_slot(this, popup_callback_);
            state_ = LiveSessionBus::instance().current_state();
            active_ab_slot_ = LiveSessionBus::instance().active_ab_slot();
            sync_controls_from_state();
            Invalidate(FALSE);
            break;
        case IDC_BTN_AB_COPY:
            LiveSessionBus::instance().copy_ab_to_other();
            break;
        case IDC_COMBO_MODE:
            if (code == CBN_SELCHANGE) {
                int sel = static_cast<int>(::SendMessageW(combo_mode_, CB_GETCURSEL, 0, 0));
                state_.processing_mode = (sel == 1) ? super_eq::ProcessingMode::LinearPhaseFIR
                                                    : super_eq::ProcessingMode::MinimumPhaseIIR;
                push_state_change(false);
            }
            break;
        case IDC_BTN_SPLIT:
            split_lr_view_ = !split_lr_view_;
            ::SetWindowTextW(btn_split_, split_lr_view_ ? L"View: Split L/R" : L"View: Overlay");
            Invalidate(FALSE);
            break;
        case IDC_COMBO_CH_FILTER:
            if (code == CBN_SELCHANGE) {
                int sel = static_cast<int>(::SendMessageW(combo_ch_filter_, CB_GETCURSEL, 0, 0));
                ch_filter_ = static_cast<ViewChannelFilter>(sel);
                Invalidate(FALSE);
            }
            break;
        case IDC_CHK_LINK_LR:
            state_.link_lr = (::SendMessageW(chk_link_lr_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            push_state_change(false);
            break;
        case IDC_BTN_COPY_L2R:
            state_.copy_channel_bands(super_eq::ChannelRoute::Left, super_eq::ChannelRoute::Right);
            push_state_change(false);
            break;
        case IDC_BTN_COPY_R2L:
            state_.copy_channel_bands(super_eq::ChannelRoute::Right, super_eq::ChannelRoute::Left);
            push_state_change(false);
            break;
        case IDC_BTN_SWAP_LR:
            state_.swap_lr();
            push_state_change(false);
            sync_controls_from_state();
            break;
        case IDC_CHK_AUTOGAIN:
            state_.auto_gain = (::SendMessageW(chk_autogain_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            push_state_change(false);
            break;
        case IDC_CHK_LIMITER:
            state_.clip_limiter = (::SendMessageW(chk_limiter_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            push_state_change(false);
            break;
        case IDC_CHK_SPECTRUM:
            show_spectrum_ = (::SendMessageW(chk_spectrum_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            Invalidate(FALSE);
            break;
        case IDC_CHK_PHASE:
            show_phase_ = (::SendMessageW(chk_phase_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            Invalidate(FALSE);
            break;
        case IDC_COMBO_DB_SCALE:
            if (code == CBN_SELCHANGE) {
                static constexpr double kScales[5] = {3.0, 6.0, 12.0, 18.0, 30.0};
                int sel = static_cast<int>(::SendMessageW(combo_db_scale_, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < 5) {
                    db_range_ = kScales[sel];
                    Invalidate(FALSE);
                }
            }
            break;
        case IDC_BTN_TABLE_TOGGLE:
            show_table_ = !show_table_;
            ::SetWindowTextW(btn_table_toggle_, show_table_ ? L"Band Table ▴" : L"Band Table ▾");
            layout_controls();
            sync_band_table();
            Invalidate(FALSE);
            break;
        case IDC_CHK_L_PHASE:
        case IDC_CHK_R_PHASE:
        case IDC_CHK_L_MUTE:
        case IDC_CHK_R_MUTE:
        case IDC_BTN_CAL_APPLY:
            state_.global_preamp_db = read_edit_double(edit_global_pre_, state_.global_preamp_db);
            state_.left_cal.preamp_db = read_edit_double(edit_l_pre_, state_.left_cal.preamp_db);
            state_.right_cal.preamp_db = read_edit_double(edit_r_pre_, state_.right_cal.preamp_db);
            state_.left_cal.delay_ms = read_edit_double(edit_l_del_, state_.left_cal.delay_ms);
            state_.right_cal.delay_ms = read_edit_double(edit_r_del_, state_.right_cal.delay_ms);
            state_.left_cal.invert_phase = (::SendMessageW(chk_l_phase_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            state_.right_cal.invert_phase = (::SendMessageW(chk_r_phase_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            state_.left_cal.mute = (::SendMessageW(chk_l_mute_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            state_.right_cal.mute = (::SendMessageW(chk_r_mute_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            push_state_change(false);
            break;
        case IDC_BTN_BAND_ADD:
            if (state_.bands.size() < super_eq::kMaxBands) {
                super_eq::BandParams nb;
                if (ch_filter_ == ViewChannelFilter::Left) nb.route = super_eq::ChannelRoute::Left;
                else if (ch_filter_ == ViewChannelFilter::Right) nb.route = super_eq::ChannelRoute::Right;
                else if (ch_filter_ == ViewChannelFilter::Mid) nb.route = super_eq::ChannelRoute::Mid;
                else if (ch_filter_ == ViewChannelFilter::Side) nb.route = super_eq::ChannelRoute::Side;
                state_.bands.push_back(nb);
                selected_band_ = static_cast<int>(state_.bands.size()) - 1;
                push_state_change(false);
            }
            break;
        case IDC_BTN_BAND_DEL:
            if (selected_band_ >= 0 && selected_band_ < static_cast<int>(state_.bands.size())) {
                state_.bands.erase(state_.bands.begin() + selected_band_);
                if (selected_band_ >= static_cast<int>(state_.bands.size())) {
                    selected_band_ = static_cast<int>(state_.bands.size()) - 1;
                }
                push_state_change(false);
            }
            break;
        case IDC_BTN_BAND_RESET:
            state_ = super_eq::EQPresetState::create_default_10band();
            selected_band_ = -1;
            push_state_change(false);
            sync_controls_from_state();
            break;
        case IDC_BTN_SEL_APPLY:
        case IDC_CHK_SEL_ON:
        case IDC_CHK_SEL_SOLO:
        case IDC_COMBO_SEL_ROUTE:
        case IDC_COMBO_SEL_TYPE:
        case IDC_COMBO_SEL_SLOPE:
            if (selected_band_ >= 0 && selected_band_ < static_cast<int>(state_.bands.size())) {
                if (id >= IDC_COMBO_SEL_ROUTE && id <= IDC_COMBO_SEL_SLOPE && code != CBN_SELCHANGE) {
                    break;
                }
                auto& b = state_.bands[static_cast<size_t>(selected_band_)];
                b.enabled = (::SendMessageW(chk_sel_on_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                b.solo = (::SendMessageW(chk_sel_solo_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                b.route = static_cast<super_eq::ChannelRoute>(::SendMessageW(combo_sel_route_, CB_GETCURSEL, 0, 0));
                b.type = static_cast<super_eq::FilterType>(::SendMessageW(combo_sel_type_, CB_GETCURSEL, 0, 0));
                const super_eq::FilterSlope slopes[] = {
                    super_eq::FilterSlope::dB6, super_eq::FilterSlope::dB12, super_eq::FilterSlope::dB18,
                    super_eq::FilterSlope::dB24, super_eq::FilterSlope::dB36, super_eq::FilterSlope::dB48
                };
                int sidx = static_cast<int>(::SendMessageW(combo_sel_slope_, CB_GETCURSEL, 0, 0));
                if (sidx >= 0 && sidx < 6) b.slope = slopes[sidx];
                b.freq_hz = read_edit_double(edit_sel_freq_, b.freq_hz);
                b.gain_db = read_edit_double(edit_sel_gain_, b.gain_db);
                b.q = read_edit_double(edit_sel_q_, b.q);
                push_state_change(false);
            }
            break;
        default:
            break;
    }
    return 0;
}

LRESULT SuperEqEditorView::OnNotify(UINT, WPARAM, LPARAM lParam, BOOL&) {
    auto* hdr = reinterpret_cast<NMHDR*>(lParam);
    if (hdr && hdr->hwndFrom == list_bands_ && hdr->code == LVN_ITEMCHANGED && !updating_controls_) {
        auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
        if ((nmlv->uNewState & LVIS_SELECTED) != 0) {
            selected_band_ = nmlv->iItem;
            sync_selected_band_controls();
            render_canvas();
        }
    }
    return 0;
}

LRESULT SuperEqEditorView::OnScroll(UINT, WPARAM, LPARAM, BOOL&) {
    return 0;
}

} // namespace super_eq_fb2k
