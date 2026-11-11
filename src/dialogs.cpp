#include "app.hpp"
#include "formats.hpp"

#include <commdlg.h>
#include <stdexcept>

namespace pf {
namespace {
struct Dialog {
    const std::wstring& title;
    std::span<const Field> fields;
    std::vector<HWND> inputs;
    std::optional<std::vector<std::wstring>> result;
    HFONT font{};
    float dpi = 1;
};

LRESULT CALLBACK dialogProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        d = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
    }
    if (msg == WM_CREATE) {
        auto child = [&](const wchar_t* type, const wchar_t* value, DWORD style, DWORD ex, int x, int y, int w, int h, int id) {
            auto scale = [d](int n) { return int(n * d->dpi); };
            auto control = CreateWindowExW(ex, type, value, style | WS_CHILD | WS_VISIBLE,
                scale(x), scale(y), scale(w), scale(h), window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(d->font), TRUE);
            return control;
        };
        int y = 16;
        for (std::size_t i = 0; i < d->fields.size(); ++i) {
            const auto& field = d->fields[i];
            child(L"STATIC", field.label.c_str(), 0, 0, 18, y, 324, 20, 0);
            y += 22;
            const auto flags = field.multiline ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN : ES_AUTOHSCROLL;
            auto input = child(L"EDIT", field.value.c_str(), WS_TABSTOP | flags, WS_EX_CLIENTEDGE, 18, y, 324,
                field.multiline ? 78 : 28, int(100 + i));
            SendMessageW(input, EM_SETLIMITTEXT, field.multiline ? 4096 : 256, 0);
            d->inputs.push_back(input);
            y += field.multiline ? 89 : 39;
        }
        child(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 166, y + 4, 84, 29, IDOK);
        child(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 258, y + 4, 84, 29, IDCANCEL);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        d->result.emplace();
        for (auto input : d->inputs) {
            const int n = GetWindowTextLengthW(input);
            std::wstring value(std::size_t(n) + 1, L'\0');
            GetWindowTextW(input, value.data(), n + 1);
            value.resize(n);
            d->result->push_back(std::move(value));
        }
        DestroyWindow(window);
        return 0;
    }
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        DestroyWindow(window);
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC) {
        auto dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(230, 230, 233));
        SetBkColor(dc, RGB(42, 43, 47));
        static HBRUSH background = CreateSolidBrush(RGB(42, 43, 47));
        return reinterpret_cast<LRESULT>(background);
    }
    return DefWindowProcW(window, msg, wp, lp);
}
} // namespace

int readNumber(const std::wstring& text, int low, int high) {
    try {
        std::size_t end{};
        const int value = std::stoi(text, &end);
        if (end == text.size() && value >= low && value <= high) return value;
    } catch (const std::invalid_argument&) {} catch (const std::out_of_range&) {}
    throw std::runtime_error("The number is outside the allowed range.");
}

std::optional<std::vector<std::wstring>> prompt(HWND owner, const std::wstring& title, std::span<const Field> fields) {
    const auto instance = GetModuleHandleW(nullptr);
    static const auto registered = [instance] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = dialogProc;
        wc.hInstance = instance;
        wc.hbrBackground = CreateSolidBrush(RGB(42, 43, 47));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PixelForgeDialog";
        return RegisterClassW(&wc);
    }();
    if (!registered) return std::nullopt;
    const UINT dpi = GetDpiForWindow(owner);
    Dialog data{title, fields, {}, {}, CreateFontW(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI"), dpi / 96.0f};
    int h = 61;
    for (const auto& field : fields) h += field.multiline ? 111 : 61;
    RECT dims{0, 0, MulDiv(360, dpi, 96), MulDiv(h, dpi, 96)};
    AdjustWindowRectExForDpi(&dims, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT parent{};
    GetWindowRect(owner, &parent);
    const auto w = dims.right - dims.left, height = dims.bottom - dims.top;
    auto window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PixelForgeDialog", title.c_str(), WS_CAPTION | WS_SYSMENU,
        (parent.left + parent.right - w) / 2, (parent.top + parent.bottom - height) / 2,
        w, height, owner, nullptr, instance, &data);
    if (window) {
        EnableWindow(owner, FALSE);
        ShowWindow(window, SW_SHOW);
        if (!data.inputs.empty()) {
            SetFocus(data.inputs.front());
            SendMessageW(data.inputs.front(), EM_SETSEL, 0, -1);
        }
        MSG message{};
        BOOL status = 1;
        while (IsWindow(window) && (status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        if (status == 0) { DestroyWindow(window); PostQuitMessage(int(message.wParam)); }
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    DeleteObject(data.font);
    return std::move(data.result);
}
std::filesystem::path fileDialog(HWND owner, bool save, bool exporting, const std::wstring& name) {
    constexpr wchar_t openTypes[] = L"Images and projects\0*.pxf;*.psd;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif;*.ico;*.webp\0All files\0*.*\0";
    constexpr wchar_t projectTypes[] = L"PixelForge Project\0*.pxf\0";
    constexpr wchar_t exportTypes[] = L"PNG image\0*.png\0JPEG image\0*.jpg\0TIFF image\0*.tif\0Bitmap image\0*.bmp\0Photoshop PSD\0*.psd\0";
    std::array<wchar_t, 32768> path{};
    std::copy_n(name.c_str(), std::min(name.size(), path.size() - 1), path.data());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = owner;
    dialog.lpstrFilter = save ? (exporting ? exportTypes : projectTypes) : openTypes;
    dialog.lpstrFile = path.data(); dialog.nMaxFile = DWORD(path.size());
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
        (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    dialog.lpstrDefExt = save ? (exporting ? L"png" : L"pxf") : nullptr;
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return {};
    return std::filesystem::path(path.data());
}
void drawType(Layer& layer, Rect clip, Point p, const std::wstring& text, int size, Pixel color,
    const wchar_t* face, int weight) {
    if (text.empty() || clip.empty()) return;
    // GDI produces an antialiased coverage mask. Keep the target allocation limited to the text bounds.
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) throw std::runtime_error("Unable to create the text renderer.");
    HFONT font = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    if (!font) { DeleteDC(dc); throw std::runtime_error("Unable to create the font."); }
    auto previousFont = SelectObject(dc, font);
    RECT metrics{0, 0, 0, 0};
    DrawTextW(dc, text.data(), int(text.size()), &metrics, DT_CALCRECT | DT_NOPREFIX);
    Rect area = intersect({p.x, p.y, int(metrics.right + 4), int(metrics.bottom + 2)}, clip);
    HBITMAP bitmap = nullptr;
    try {
        if (!area.empty()) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = area.w; info.bmiHeader.biHeight = -area.h;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            void* buffer{};
            bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &buffer, nullptr, 0);
            if (!bitmap) throw std::runtime_error("Unable to allocate the text mask.");
            auto oldBitmap = SelectObject(dc, bitmap);
            PatBlt(dc, 0, 0, area.w, area.h, BLACKNESS);
            SetTextColor(dc, RGB(255, 255, 255)); SetBkMode(dc, TRANSPARENT);
            RECT textRect{p.x - area.x, p.y - area.y, p.x - area.x + metrics.right + 4, p.y - area.y + metrics.bottom + 2};
            DrawTextW(dc, text.data(), int(text.size()), &textRect, DT_NOPREFIX | DT_NOCLIP);
            GdiFlush();
            SelectObject(dc, oldBitmap);
            auto pixels = std::span(static_cast<Pixel*>(buffer), std::size_t(area.w) * area.h);
            for (auto& pixel : pixels) pixel = (color & 0xffffffu) | (Pixel((blue(pixel) * alpha(color) + 127) / 255) << 24);
            importPixels(layer.pixels, {area.x - layer.x, area.y - layer.y, area.w, area.h}, pixels, area.w);
        }
    } catch (...) {
        if (bitmap) DeleteObject(bitmap);
        SelectObject(dc, previousFont); DeleteObject(font); DeleteDC(dc);
        throw;
    }
    if (bitmap) DeleteObject(bitmap);
    SelectObject(dc, previousFont); DeleteObject(font); DeleteDC(dc);
}
} // namespace pf
