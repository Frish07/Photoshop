#include "app.hpp"

#include <commdlg.h>
#include <dwmapi.h>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pf {
namespace {
constexpr int catFirst = 100, cardFirst = 120, openId = 140, landscapeId = 141, portraitId = 142;
constexpr int unitId = 150, backgroundId = 151;
constexpr COLORREF body = RGB(32, 32, 32), panel = RGB(40, 40, 40), field = RGB(52, 52, 52);
constexpr COLORREF ink = RGB(242, 242, 242), gray = RGB(178, 178, 178), accent = RGB(42, 117, 245);

struct Preset { const wchar_t* name; int w, h, ppi; };
constexpr const wchar_t* categories[]{L"Photo", L"Print", L"Illustration", L"Web", L"Mobile", L"Film & Video"};
constexpr Preset presets[][6]{
    {{L"Landscape", 6000, 4000, 300}, {L"Portrait", 4000, 6000, 300}, {L"Photo 4 × 6", 1800, 1200, 300},
        {L"Photo 5 × 7", 2100, 1500, 300}, {L"Photo 8 × 10", 3000, 2400, 300}, {L"Square", 3000, 3000, 300}},
    {{L"A4", 2480, 3508, 300}, {L"A3", 3508, 4961, 300}, {L"US Letter", 2550, 3300, 300},
        {L"A5", 1748, 2480, 300}, {L"US Legal", 2550, 4200, 300}, {L"Tabloid", 3300, 5100, 300}},
    {{L"Landscape", 3000, 2000, 300}, {L"Portrait", 2000, 3000, 300}, {L"Square", 2048, 2048, 300},
        {L"Comic Page", 1988, 3075, 300}, {L"Concept Art", 3840, 2160, 150}, {L"Icon", 512, 512, 72}},
    {{L"Web 1920", 1920, 1080, 72}, {L"Web 1440", 1440, 900, 72}, {L"Web 1280", 1280, 720, 72},
        {L"Social Square", 1080, 1080, 72}, {L"Social Portrait", 1080, 1350, 72}, {L"Web Banner", 1600, 500, 72}},
    {{L"Mobile", 1080, 2400, 72}, {L"Large Mobile", 1440, 3200, 72}, {L"Tablet", 1536, 2048, 72},
        {L"Tablet Landscape", 2048, 1536, 72}, {L"Story", 1080, 1920, 72}, {L"App Icon", 1024, 1024, 72}},
    {{L"HDTV 1080p", 1920, 1080, 72}, {L"HDTV 720p", 1280, 720, 72}, {L"4K UHD", 3840, 2160, 72},
        {L"DCI 4K", 4096, 2160, 72}, {L"Vertical Video", 1080, 1920, 72}, {L"8K UHD", 7680, 4320, 72}}
};

std::wstring value(HWND field) {
    const auto n = GetWindowTextLengthW(field);
    std::wstring text(std::size_t(n) + 1, L'\0');
    GetWindowTextW(field, text.data(), n + 1);
    text.resize(n);
    return text;
}
double numeric(HWND field) {
    auto text = value(field);
    std::replace(text.begin(), text.end(), L',', L'.');
    std::size_t consumed{};
    const auto v = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(v) || v <= 0) throw std::runtime_error("Enter a positive number.");
    return v;
}
std::wstring decimal(double number, int precision) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(precision) << number;
    auto text = out.str();
    if (precision) {
        while (text.ends_with(L'0')) text.pop_back();
        if (text.ends_with(L'.')) text.pop_back();
    }
    return text;
}

struct Dialog {
    Dialog(std::wstring name, Pixel color, UINT dpi, int w, int h)
        : title(std::move(name)), background(color), scale(dpi / 96.0), clientW(w), clientH(h) {
        details = clientW - 314;
        regular = font(14, FW_NORMAL);
        bold = font(14, FW_SEMIBOLD);
        heading = font(20, FW_SEMIBOLD);
        fieldBrush = CreateSolidBrush(field);
    }
    ~Dialog() {
        DeleteObject(regular); DeleteObject(bold); DeleteObject(heading); DeleteObject(fieldBrush);
    }
    int px(int dip) const { return int(std::round(dip * scale)); }
    HFONT font(int size, int weight) const {
        return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    RECT rect(int x, int y, int w, int h) const { return {px(x), px(y), px(x + w), px(y + h)}; }
    void fill(HDC dc, RECT r, COLORREF c) const {
        const auto brush = CreateSolidBrush(c);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    }
    void text(HDC dc, RECT r, const std::wstring& s, COLORREF c, HFONT font, UINT flags = DT_LEFT) const {
        SelectObject(dc, font);
        SetTextColor(dc, c);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, s.c_str(), int(s.size()), &r, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    HWND control(const wchar_t* type, const wchar_t* title, int id, DWORD style, RECT r) const {
        auto child = CreateWindowExW(0, type, title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            r.left, r.top, r.right - r.left, r.bottom - r.top, window, reinterpret_cast<HMENU>(INT_PTR(id)),
            GetModuleHandleW(nullptr), nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(regular), TRUE);
        return child;
    }
    HWND button(const wchar_t* title, int id, RECT r) const { return control(L"BUTTON", title, id, BS_OWNERDRAW, r); }
    HWND edit(const wchar_t* text, int id, RECT r, int maxLength) const {
        auto controlWindow = control(L"EDIT", text, id, ES_AUTOHSCROLL | WS_BORDER, r);
        SendMessageW(controlWindow, EM_SETLIMITTEXT, maxLength, 0);
        SendMessageW(controlWindow, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(px(8), px(8)));
        return controlWindow;
    }
    void create() {
        constexpr int tabWidths[]{56, 51, 94, 51, 65, 102};
        int tabX = 20;
        for (int i = 0; i < int(std::size(categories)); ++i) {
            tabs[i] = button(categories[i], catFirst + i, rect(tabX, 12, tabWidths[i], 37));
            tabX += tabWidths[i] + 3;
        }
        const int cardW = (details - 50) / 3, cardH = std::min(140, (clientH - 181) / 2);
        for (int i = 0; i < int(cards.size()); ++i) {
            cards[i] = button(L"", cardFirst + i, rect(22 + i % 3 * (cardW + 4), 98 + i / 3 * (cardH + 9), cardW, cardH));
        }
        int x = details + 22;
        name = edit(title.c_str(), 160, rect(x, 87, 270, 30), 120);
        width = edit(L"", 161, rect(x, 153, 122, 30), 18);
        height = edit(L"", 162, rect(x + 139, 153, 131, 30), 18);
        units = control(L"COMBOBOX", L"", unitId, CBS_DROPDOWNLIST | WS_VSCROLL, rect(x, 196, 120, 154));
        for (auto label : {L"Pixels", L"Inches", L"Centimeters"}) SendMessageW(units, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(units, CB_SETCURSEL, 0, 0);
        landscape = button(L"Landscape", landscapeId, rect(x + 130, 196, 79, 27));
        portrait = button(L"Portrait", portraitId, rect(x + 212, 196, 58, 27));
        resolution = edit(L"", 163, rect(x, 257, 122, 30), 18);
        backgrounds = control(L"COMBOBOX", L"", backgroundId, CBS_DROPDOWNLIST | WS_VSCROLL, rect(x, 320, 270, 150));
        for (auto label : {L"White", L"Transparent", L"Background Color", L"Custom Color..."})
            SendMessageW(backgrounds, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(backgrounds, CB_SETCURSEL, 0, 0);
        button(L"Open Image...", openId, rect(22, clientH - 56, 111, 34));
        button(L"Cancel", IDCANCEL, rect(x + 85, clientH - 56, 81, 34));
        button(L"Create", IDOK, rect(x + 176, clientH - 56, 94, 34));
        setCategory(category);
    }
    void setCategory(int n) {
        category = n;
        for (int i = 0; i < int(cards.size()); ++i) SetWindowTextW(cards[i], presets[n][i].name);
        setPreset(0);
        for (auto tab : tabs) InvalidateRect(tab, nullptr, FALSE);
    }
    void setPreset(int n) {
        card = n;
        const auto preset = presets[category][card];
        SetWindowTextW(resolution, std::to_wstring(preset.ppi).c_str());
        setSize(preset.w, preset.h, preset.ppi);
        for (auto button : cards) InvalidateRect(button, nullptr, FALSE);
    }
    double conversion(double ppi) const { return unit == 0 ? 1 : unit == 1 ? ppi : ppi / 2.54; }
    void setSize(double w, double h, double ppi) {
        const auto factor = conversion(ppi);
        SetWindowTextW(width, decimal(w / factor, unit ? 4 : 0).c_str());
        SetWindowTextW(height, decimal(h / factor, unit ? 4 : 0).c_str());
        InvalidateRect(landscape, nullptr, FALSE); InvalidateRect(portrait, nullptr, FALSE);
    }
    void changeUnit() {
        const auto previous = unit;
        try {
            const auto ppi = numeric(resolution);
            const auto w = numeric(width) * conversion(ppi), h = numeric(height) * conversion(ppi);
            unit = int(SendMessageW(units, CB_GETCURSEL, 0, 0));
            setSize(w, h, ppi);
        } catch (...) {
            unit = previous;
            SendMessageW(units, CB_SETCURSEL, unit, 0);
            error = L"Enter valid dimensions before changing units.";
            InvalidateRect(window, nullptr, FALSE);
        }
    }
    void orientation(bool wantPortrait) {
        const auto w = value(width), h = value(height);
        if ((numeric(width) > numeric(height)) == wantPortrait) {
            SetWindowTextW(width, h.c_str());
            SetWindowTextW(height, w.c_str());
        }
        InvalidateRect(landscape, nullptr, FALSE); InvalidateRect(portrait, nullptr, FALSE);
    }
    void chooseColor() {
        const int selection = int(SendMessageW(backgrounds, CB_GETCURSEL, 0, 0));
        if (selection == 3) {
            COLORREF custom[16]{};
            CHOOSECOLORW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = window;
            dialog.lpCustColors = custom;
            dialog.Flags = CC_RGBINIT | CC_FULLOPEN;
            dialog.rgbResult = RGB(red(customColor), green(customColor), blue(customColor));
            if (!ChooseColorW(&dialog)) {
                SendMessageW(backgrounds, CB_SETCURSEL, colorSelection, 0);
                return;
            }
            customColor = rgba(GetRValue(dialog.rgbResult), GetGValue(dialog.rgbResult), GetBValue(dialog.rgbResult));
        }
        colorSelection = selection;
        InvalidateRect(window, nullptr, FALSE);
    }
    void finish() {
        const auto ppi = numeric(resolution);
        const auto w = numeric(width) * conversion(ppi), h = numeric(height) * conversion(ppi);
        if (ppi < 1 || ppi > 2400) throw std::runtime_error("Resolution must be 1–2400 PPI.");
        if (w < 0.5 || h < 0.5 || w >= maxEdge + 0.5 || h >= maxEdge + 0.5)
            throw std::runtime_error("Canvas edges must be 1–16,384 px.");
        auto chosenName = value(name);
        if (chosenName.empty()) chosenName = title;
        result = NewDocumentOptions{std::move(chosenName), int(std::round(w)), int(std::round(h)),
            std::round(ppi * 1000) / 1000, color()};
        DestroyWindow(window);
    }
    Pixel color() const {
        switch (colorSelection) {
        case 1: return 0;
        case 2: return background;
        case 3: return customColor;
        default: return 0xffffffff;
        }
    }
    void paint(HDC dc) const {
        fill(dc, rect(0, 0, clientW, clientH), body);
        fill(dc, rect(details, 0, 314, clientH), panel);
        fill(dc, rect(details, 0, 1, clientH), RGB(18, 18, 18));
        fill(dc, rect(0, 55, details, 1), RGB(56, 56, 56));
        text(dc, rect(22, 63, 300, 30), L"Blank Document Presets", ink, heading);
        const int x = details + 22;
        text(dc, rect(x, 20, 270, 28), L"Preset Details", ink, heading);
        text(dc, rect(x, 61, 270, 23), L"Name", gray, regular);
        text(dc, rect(x, 126, 121, 23), L"Width", gray, regular);
        text(dc, rect(x + 139, 126, 120, 23), L"Height", gray, regular);
        text(dc, rect(x, 231, 270, 22), L"Resolution", gray, regular);
        text(dc, rect(x + 132, 257, 135, 30), L"Pixels / Inch", gray, regular);
        text(dc, rect(x, 293, 270, 23), L"Background Contents", gray, regular);
        text(dc, rect(x, 356, 180, 28), L"RGB Color  ·  8 bit", gray, regular);
        if (const auto c = color())
            fill(dc, rect(x + 248, 359, 20, 20), RGB(red(c), green(c), blue(c)));
        else {
            for (int y = 0; y < 4; ++y) for (int i = 0; i < 4; ++i)
                fill(dc, rect(x + 248 + i * 5, 359 + y * 5, 5, 5), (i + y) % 2 ? RGB(135, 135, 135) : RGB(222, 222, 222));
        }
        text(dc, rect(x, clientH - 93, 270, 25), error, RGB(255, 157, 143), regular);
    }
    void draw(const DRAWITEMSTRUCT& item) const {
        const int id = int(item.CtlID);
        const auto r = item.rcItem;
        const auto dc = item.hDC;
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (id >= cardFirst && id < cardFirst + int(cards.size())) {
            const int index = id - cardFirst;
            const auto& p = presets[category][index];
            fill(dc, r, card == index ? RGB(39, 55, 75) : RGB(46, 46, 46));
            if (card == index) {
                const auto b = CreateSolidBrush(accent);
                FrameRect(dc, &r, b);
                DeleteObject(b);
            }
            const double ratio = px(51) / double(std::max(p.w, p.h));
            const int thumbW = std::max(px(14), int(p.w * ratio)), thumbH = std::max(px(14), int(p.h * ratio));
            const int centerY = h / 2 - px(17);
            fill(dc, {w / 2 - thumbW / 2, centerY - thumbH / 2, w / 2 + thumbW / 2, centerY + thumbH / 2},
                RGB(240, 240, 240));
            text(dc, {px(4), h - px(55), w - px(4), h - px(32)}, p.name, ink, bold, DT_CENTER);
            text(dc, {px(4), h - px(32), w - px(4), h - px(10)},
                std::to_wstring(p.w) + L" × " + std::to_wstring(p.h) + L" px", gray, regular, DT_CENTER);
        } else if (id >= catFirst && id < catFirst + int(tabs.size())) {
            fill(dc, r, body);
            const bool on = id - catFirst == category;
            if (on) fill(dc, {px(5), h - px(2), w - px(5), h}, accent);
            text(dc, r, categories[id - catFirst], on ? ink : gray, on ? bold : regular, DT_CENTER);
        } else {
            const bool active = id == IDOK || (item.itemState & ODS_SELECTED);
            bool orientationSelected = false;
            if (id == portraitId || id == landscapeId) {
                try { orientationSelected = (numeric(width) < numeric(height)) == (id == portraitId); }
                catch (...) {}
            }
            fill(dc, r, active ? accent : orientationSelected ? RGB(56, 69, 87) : RGB(62, 62, 62));
            text(dc, r, value(item.hwndItem), ink, id == IDOK ? bold : regular, DT_CENTER);
        }
        if (item.itemState & ODS_FOCUS) {
            RECT focused = r;
            InflateRect(&focused, -px(3), -px(3));
            DrawFocusRect(dc, &focused);
        }
    }

    std::wstring title, error;
    Pixel background, customColor = 0xffffffff;
    double scale;
    int clientW, clientH, details{}, category = 3, card{}, unit{}, colorSelection{};
    HWND window{}, name{}, width{}, height{}, units{}, landscape{}, portrait{}, resolution{}, backgrounds{};
    std::array<HWND, 6> tabs{}, cards{};
    HFONT regular{}, bold{}, heading{};
    HBRUSH fieldBrush{};
    std::optional<NewDocumentOptions> result;
};

LRESULT CALLBACK windowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* data = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        data = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        data->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    if (!data) return DefWindowProcW(window, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        try { data->create(); return 0; }
        catch (...) { return -1; }
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        const auto dc = BeginPaint(window, &ps);
        data->paint(dc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DRAWITEM: data->draw(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSTATIC: {
        const auto dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, ink);
        SetBkColor(dc, field);
        return reinterpret_cast<LRESULT>(data->fieldBrush);
    }
    case DM_GETDEFID: return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_COMMAND:
        try {
            const int id = LOWORD(wp), note = HIWORD(wp);
            if (note == EN_CHANGE && (id == 161 || id == 162)) {
                if (data->landscape) InvalidateRect(data->landscape, nullptr, FALSE);
                if (data->portrait) InvalidateRect(data->portrait, nullptr, FALSE);
            }
            if (id == IDOK) data->finish();
            else if (id == IDCANCEL) DestroyWindow(window);
            else if (id == openId) {
                data->result = NewDocumentOptions{};
                data->result->openExisting = true;
                DestroyWindow(window);
            } else if (id >= catFirst && id < catFirst + int(data->tabs.size())) data->setCategory(id - catFirst);
            else if (id >= cardFirst && id < cardFirst + int(data->cards.size())) data->setPreset(id - cardFirst);
            else if (id == landscapeId || id == portraitId) data->orientation(id == portraitId);
            else if (note == CBN_SELCHANGE && id == unitId) data->changeUnit();
            else if (note == CBN_SELCHANGE && id == backgroundId) data->chooseColor();
        } catch (...) {
            data->error = L"Enter dimensions up to 16,384 px and 1–2400 PPI.";
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
} // namespace

std::optional<NewDocumentOptions> newDocumentDialog(HWND owner, std::wstring title, Pixel background) {
    const auto instance = GetModuleHandleW(nullptr);
    static const auto registered = [instance] {
        WNDCLASSW cls{};
        cls.lpfnWndProc = windowProc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = L"PixelForgeNewDocument";
        return RegisterClassW(&cls);
    }();
    if (!registered) throw std::runtime_error("Unable to register the New Document window.");
    const auto dpi = GetDpiForWindow(owner);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto& work = monitor.rcWork;
    const int availableW = MulDiv(work.right - work.left, 96, dpi);
    const int availableH = MulDiv(work.bottom - work.top, 96, dpi);
    Dialog data(std::move(title), background, dpi, std::clamp(availableW - 36, 785, 960),
        std::clamp(availableH - 55, 485, 548));
    RECT dims{0, 0, data.px(data.clientW), data.px(data.clientH)};
    AdjustWindowRectExForDpi(&dims, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    const int w = dims.right - dims.left, h = dims.bottom - dims.top;
    auto window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PixelForgeNewDocument", L"New Document", WS_CAPTION | WS_SYSMENU,
        (work.left + work.right - w) / 2, (work.top + work.bottom - h) / 2, w, h, owner, nullptr, instance, &data);
    if (!window) throw std::runtime_error("Unable to open the New Document window.");
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(data.name);
    SendMessageW(data.name, EM_SETSEL, 0, -1);
    MSG msg{};
    BOOL status = 1;
    while (IsWindow(window) && (status = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (IsWindow(window)) DestroyWindow(window);
    if (status == 0) PostQuitMessage(int(msg.wParam));
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return std::move(data.result);
}
} // namespace pf
