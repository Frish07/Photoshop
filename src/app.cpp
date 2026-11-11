#include "app.hpp"
#include "formats.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cmath>
#include <stdexcept>

namespace pf {
namespace {
struct MenuItem { Command command; const wchar_t* label; };
constexpr MenuItem fileMenu[] = {{Command::newDoc, L"New...\tCtrl+N"}, {Command::open, L"Open...\tCtrl+O"},
    {Command::demo, L"Open Sample"}, {Command::none, nullptr}, {Command::save, L"Save\tCtrl+S"},
    {Command::saveAs, L"Save As...\tCtrl+Shift+S"}, {Command::exportFile, L"Export...\tCtrl+Alt+Shift+S"},
    {Command::none, nullptr}, {Command::close, L"Close\tCtrl+W"}, {Command::quit, L"Exit\tAlt+F4"}};
constexpr MenuItem editMenu[] = {{Command::undo, L"Undo\tCtrl+Z"}, {Command::redo, L"Redo\tCtrl+Shift+Z"},
    {Command::none, nullptr}, {Command::cut, L"Cut\tCtrl+X"}, {Command::copy, L"Copy\tCtrl+C"},
    {Command::copyMerged, L"Copy Merged\tCtrl+Shift+C"},
    {Command::paste, L"Paste\tCtrl+V"}, {Command::clear, L"Clear\tDelete"},
    {Command::fillFG, L"Fill Foreground\tAlt+Backspace"}, {Command::fillBG, L"Fill Background\tCtrl+Backspace"},
    {Command::none, nullptr}, {Command::freeTransform, L"Free Transform\tCtrl+T"},
    {Command::shortcuts, L"Keyboard Shortcuts...\tCtrl+Alt+Shift+K"}};
constexpr MenuItem imageMenu[] = {{Command::resize, L"Image Size...\tCtrl+Alt+I"}, {Command::canvasSize, L"Canvas Size...\tCtrl+Alt+C"},
    {Command::resolution, L"Resolution..."},
    {Command::rotate, L"Rotate Canvas 90° Clockwise"},
    {Command::flipX, L"Flip Canvas Horizontal"}, {Command::flipY, L"Flip Canvas Vertical"},
    {Command::cropSelection, L"Crop to Selection"}};
constexpr MenuItem layerMenu[] = {{Command::newLayerDialog, L"New Layer...\tCtrl+Shift+N"}, {Command::place, L"Place Image..."},
    {Command::duplicate, L"Layer via Copy\tCtrl+J"}, {Command::layerViaCut, L"Layer via Cut\tCtrl+Shift+J"},
    {Command::rename, L"Rename Layer..."}, {Command::deleteLayer, L"Delete Layer"},
    {Command::none, nullptr}, {Command::layerUp, L"Bring Forward\tCtrl+]"}, {Command::layerDown, L"Send Backward\tCtrl+["},
    {Command::toggleLock, L"Toggle Layer Lock"}, {Command::addMask, L"Add Layer Mask"}, {Command::removeMask, L"Delete Layer Mask"},
    {Command::none, nullptr}, {Command::editAdjustment, L"Edit Adjustment..."},
    {Command::mergeDown, L"Merge Down\tCtrl+E"}, {Command::mergeVisible, L"Merge Visible\tCtrl+Shift+E"}, {Command::flatten, L"Flatten Image"}};
constexpr MenuItem adjustmentsMenu[] = {{Command::addTone, L"Brightness/Contrast..."}, {Command::addLevels, L"Levels..."},
    {Command::addHue, L"Hue/Saturation..."}};
constexpr MenuItem stylesMenu[] = {{Command::strokeStyle, L"Stroke..."}, {Command::overlayStyle, L"Color Overlay..."},
    {Command::shadowStyle, L"Drop Shadow..."}, {Command::clearStyle, L"Clear Layer Style"}};
constexpr MenuItem selectMenu[] = {{Command::selectAll, L"All\tCtrl+A"}, {Command::deselect, L"Deselect\tCtrl+D"},
    {Command::reselect, L"Reselect\tCtrl+Shift+D"}, {Command::inverseSelection, L"Inverse\tCtrl+Shift+I"},
    {Command::none, nullptr}, {Command::loadSelection, L"Load Layer Transparency"}};
constexpr MenuItem filterMenu[] = {{Command::brightness, L"Brightness..."}, {Command::contrast, L"Contrast..."},
    {Command::gray, L"Desaturate\tCtrl+Shift+U"}, {Command::invert, L"Invert\tCtrl+I"},
    {Command::none, nullptr}, {Command::blur, L"Box Blur..."}};
constexpr MenuItem viewMenu[] = {{Command::fit, L"Fit on Screen\tCtrl+0"}, {Command::actual, L"100%\tCtrl+1"},
    {Command::zoomIn, L"Zoom In\tCtrl++"}, {Command::zoomOut, L"Zoom Out\tCtrl+-"}, {Command::rulers, L"Rulers\tCtrl+R"},
    {Command::extras, L"Extras\tCtrl+H"}, {Command::snap, L"Snap\tCtrl+Shift+;"}, {Command::none, nullptr},
    {Command::guides, L"Show Guides\tCtrl+;"}, {Command::lockGuides, L"Lock Guides\tCtrl+Alt+;"},
    {Command::newGuide, L"New Guide..."}, {Command::clearGuides, L"Clear Guides"},
    {Command::none, nullptr}, {Command::compare, L"Quick Compare\t\\"}};
constexpr MenuItem screenMenu[] = {{Command::screenStandard, L"Standard Screen Mode"},
    {Command::screenMenus, L"Full Screen Mode With Menu Bar"}, {Command::screenFull, L"Full Screen Mode"}};
constexpr MenuItem windowMenu[] = {{Command::panels, L"Show Panels\tTab"}, {Command::rightPanels, L"Show Right Panels\tShift+Tab"},
    {Command::contextBar, L"Contextual Task Bar"}};
constexpr MenuItem helpMenu[] = {{Command::about, L"About PixelForge"}};
constexpr std::span<const MenuItem> menus[]{fileMenu, editMenu, imageMenu, layerMenu, selectMenu, filterMenu,
    viewMenu, windowMenu, helpMenu};

bool pressed(int key) { return (GetKeyState(key) & 0x8000) != 0; }
bool control() { return pressed(VK_CONTROL); }
bool shift() { return pressed(VK_SHIFT); }
bool alt() { return pressed(VK_MENU); }

} // namespace

Tab::Tab(State state, std::wstring name, std::wstring history)
    : doc(std::move(state), std::move(history)), title(std::move(name)) {}

App::App(HINSTANCE instance) : instance_(instance) { updateHSV(); }

bool App::create(int show) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.hInstance = instance_;
    wc.lpfnWndProc = windowProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(102));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"PixelForgeEditor";
    if (!RegisterClassExW(&wc)) return false;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const float scale = GetDpiForSystem() / 96.0f;
    const auto w = std::min<LONG>(LONG(1490 * scale), work.right - work.left);
    const auto h = std::min<LONG>(LONG(900 * scale), work.bottom - work.top);
    window_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"PixelForge", WS_OVERLAPPEDWINDOW,
        (work.left + work.right - w) / 2, (work.top + work.bottom - h) / 2, w, h, nullptr, nullptr, instance_, this);
    if (!window_) return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    COLORREF border = RGB(42, 42, 42), caption = RGB(32, 32, 32);
    DwmSetWindowAttribute(window_, 34, &border, sizeof(border));
    DwmSetWindowAttribute(window_, 35, &caption, sizeof(caption));
    ShowWindow(window_, show);
    return true;
}
LRESULT CALLBACK App::windowProc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, msg, w, l);
    try {
        return app->message(msg, w, l);
    } catch (const std::exception& error) {
        app->cancelDrag();
        if (auto t = app->tab(); t && t->doc.inEdit()) t->doc.cancel();
        app->documentChanged();
        app->reportError(error);
        return 0;
    }
}
LRESULT App::message(UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: dpi_ = GetDpiForWindow(window_) / 96.0f; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(window_, &ps);
        try { paint(); }
        catch (...) { EndPaint(window_, &ps); throw; }
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_SIZE:
        if (target_ && LOWORD(l) && HIWORD(l)) target_->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        repaint();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(l);
        if (screenMode_ == 0) info->ptMinTrackSize = {LONG(850 * dpi_), LONG(560 * dpi_)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = LOWORD(w) / 96.0f;
        if (target_) target_->SetDpi(float(LOWORD(w)), float(HIWORD(w)));
        const auto* suggested = reinterpret_cast<RECT*>(l);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        repaint();
        return 0;
    }
    case WM_LBUTTONDOWN: leftDown(mouse(l)); return 0;
    case WM_LBUTTONDBLCLK: {
        const auto p = mouse(l);
        if (tool_ == Tool::polygonLasso && !lassoPath_.empty() && inside(viewport_, p)) {
            finishLasso();
            return 0;
        }
        if (tab()) for (auto i = hits_.rbegin(); i != hits_.rend(); ++i) {
            if (!inside(i->rect, p)) continue;
            if (i->kind == Hit::layer || i->kind == Hit::layerThumbnail) {
                tab()->doc.select(i->id);
                tab()->mask = false;
                command(tab()->doc.active().adjustment ? Command::editAdjustment : Command::rename);
                repaint();
                return 0;
            }
            break;
        }
        leftDown(p);
        return 0;
    }
    case WM_MBUTTONDOWN: leftDown(mouse(l), true); return 0;
    case WM_LBUTTONUP: case WM_MBUTTONUP: leftUp(mouse(l)); return 0;
    case WM_RBUTTONUP: {
        if (drag_ != Drag::none) return 0;
        const auto p = mouse(l);
        for (auto i = hits_.rbegin(); i != hits_.rend(); ++i) {
            if (!inside(i->rect, p)) continue;
            if (i->kind == Hit::tool) {
                const auto tool = std::find_if(std::begin(tools), std::end(tools), [&](const ToolInfo& item) { return item.tool == Tool(i->id); });
                if (tool != std::end(tools)) popupTools({i->rect.right, i->rect.top}, tool->key);
                break;
            }
            if (i->kind == Hit::layer || i->kind == Hit::layerThumbnail) {
                tab()->doc.select(i->id);
                tab()->mask = false;
                resetTransform();
                menu(3, p, true);
                break;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: moveMouse(mouse(l)); return 0;
    case WM_MOUSELEAVE: overWindow_ = false; cursor_ = {-100, -100}; tooltip_ = false; repaint(); return 0;
    case WM_CAPTURECHANGED: if (drag_ != Drag::none) cancelDrag(); return 0;
    case WM_TIMER:
        if (w == 1) { KillTimer(window_, 1); tooltip_ = true; }
        repaint(); return 0;
    case WM_MOUSEWHEEL: {
        if (drag_ != Drag::none) return 0;
        auto t = tab();
        if (!t) return 0;
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(window_, &point);
        Vec p{point.x / dpi_, point.y / dpi_};
        const int steps = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        if (inside(viewport_, p)) {
            if (shift()) { t->pan.x += steps * 40.0f; t->fit = false; }
            else zoomAt(t->zoom * std::pow(1.18f, float(steps)), p);
        } else if (inside(sidebar_, p)) {
            if (sidebarTab_ == 0) t->layerScroll = std::max(0, t->layerScroll - steps * 2);
            else t->historyScroll = std::max(0, t->historyScroll + steps * 2);
        } else if (p.y >= 68 && p.y < 101) {
            tabFirst_ = std::clamp(tabFirst_ - steps, 0, int(tabs_.size()) - 1);
        }
        repaint();
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: if (key(w)) return 0; break;
    case WM_KEYUP: if (w == VK_SPACE) { space_ = false; repaint(); } return 0;
    case WM_KILLFOCUS: space_ = false; return 0;
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(w);
        const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        std::vector<std::filesystem::path> paths;
        for (UINT i = 0; i < count; ++i) {
            const auto len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(len + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), len + 1);
            path.resize(len);
            paths.emplace_back(path);
        }
        DragFinish(drop);
        for (const auto& path : paths) {
            try { openPath(path); } catch (const std::exception& error) { reportError(error); }
        }
        return 0;
    }
    case WM_CLOSE:
        if (drag_ != Drag::none) cancelDrag();
        while (!tabs_.empty()) {
            if (!closeTab(current_)) return 0;
        }
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window_, msg, w, l);
}

void App::repaint() { InvalidateRect(window_, nullptr, FALSE); }
void App::clearCache() { bitmaps_.clear(); beforeBitmaps_.clear(); thumbs_.clear(); }
void App::invalidate(Rect dirty) {
    if (!dirty.empty() && tab()) {
        const auto radius = tab()->doc.active().style.padding();
        dirty = {dirty.x - radius, dirty.y - radius, dirty.w + radius * 2, dirty.h + radius * 2};
    }
    std::erase_if(bitmaps_, [&](const auto& entry) { return !intersect(entry.second.area, dirty).empty(); });
    repaint();
}
void App::documentChanged(bool pixels) {
    if (pixels) clearCache();
    if (auto t = tab()) {
        if (!t->doc.active().mask) t->mask = false;
        if (t->cloneSource && !t->doc.state().bounds().contains(*t->cloneSource)) t->cloneSource.reset();
        if (t->doc.historyStartId() != compareId_) { beforeBitmaps_.clear(); compareId_ = t->doc.historyStartId(); }
        memory_ = t->doc.pixelBytes();
    } else memory_ = 0;
    updateTitle();
    repaint();
}
void App::updateTitle() {
    auto t = tab();
    const std::wstring title = t ? t->title + (t->doc.dirty() ? L" *" : L"") + L"  —  PixelForge" : L"PixelForge";
    SetWindowTextW(window_, title.c_str());
}
void App::reportError(const std::exception& error) {
    MessageBoxA(window_, error.what(), "PixelForge", MB_OK | MB_ICONWARNING);
}
void App::addTab(State state, std::wstring title, std::wstring action) {
    tabs_.push_back(std::make_unique<Tab>(std::move(state), std::move(title), std::move(action)));
    switchTab(tabs_.size() - 1);
}
void App::switchTab(std::size_t index) {
    if (index >= tabs_.size()) return;
    current_ = index;
    lassoPath_.clear();
    const int visible = std::max(1, int(width(viewport_)) / 206);
    if (int(index) < tabFirst_) tabFirst_ = int(index);
    if (int(index) >= tabFirst_ + visible) tabFirst_ = int(index) - visible + 1;
    documentChanged();
}
bool App::saveTab(Tab& t, bool as) {
    auto path = t.path;
    if (as || path.empty()) path = fileDialog(window_, true, false, t.title);
    if (path.empty()) return false;
    if (_wcsicmp(path.extension().c_str(), L".pxf") != 0) {
        MessageBoxW(window_, L"Use the .pxf extension to save all editable layers. For other formats, choose File → Export.",
            L"PixelForge", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    save(t.doc.state(), path);
    t.path = path;
    t.title = path.filename().wstring();
    t.doc.markSaved();
    documentChanged(false);
    return true;
}
bool App::closeTab(std::size_t index) {
    if (index >= tabs_.size()) return true;
    auto& t = *tabs_[index];
    if (t.doc.dirty()) {
        const auto choice = MessageBoxW(window_, (L"Save changes to " + t.title + L"?").c_str(),
            L"PixelForge", MB_ICONQUESTION | MB_YESNOCANCEL);
        if (choice == IDCANCEL || (choice == IDYES && !saveTab(t, false))) return false;
    }
    tabs_.erase(tabs_.begin() + index);
    if (index < current_) --current_;
    if (tabs_.empty()) { current_ = 0; tabFirst_ = 0; setScreenMode(0); documentChanged(); }
    else {
        tabFirst_ = std::min(tabFirst_, int(tabs_.size()) - 1);
        switchTab(std::min(current_, tabs_.size() - 1));
    }
    return true;
}
void App::openPath(const std::filesystem::path& path, bool placed) {
    auto incoming = open(path);
    if (placed && tab()) {
        auto& d = tab()->doc;
        if (d.state().layers.size() >= maxLayers) throw std::runtime_error("This document has reached its layer limit.");
        Layer layer;
        if (incoming.layers.size() == 1 && !incoming.layers.front().mask && incoming.layers.front().opacity == 255 &&
            incoming.layers.front().visible && incoming.layers.front().x == 0 && incoming.layers.front().y == 0) {
            layer.pixels = std::move(incoming.layers.front().pixels);
        } else {
            constexpr int batch = 32;
            std::vector<Pixel> band(std::size_t(incoming.width) * batch);
            for (int y = 0; y < incoming.height; y += batch) {
                const int h = std::min(batch, incoming.height - y);
                composite(incoming, {0, y, incoming.width, h}, band, incoming.width);
                for (int i = 0; i < h * incoming.width; ++i) band[i] = straight(band[i]);
                importPixels(layer.pixels, {0, y, incoming.width, h}, band, incoming.width);
            }
        }
        layer.name = path.stem().wstring();
        layer.x = (d.state().width - incoming.width) / 2;
        layer.y = (d.state().height - incoming.height) / 2;
        d.begin();
        auto& state = d.edit();
        state.layers.insert(state.layers.begin() + ++state.active, std::move(layer));
        d.commit(L"Place Image");
        tab()->mask = false;
        tab()->layerScroll = 0;
        documentChanged();
        return;
    }
    addTab(std::move(incoming), path.filename().wstring(), L"Open");
    if (_wcsicmp(path.extension().c_str(), L".pxf") == 0) tab()->path = path;
}
void App::openDemo() {
    auto state = demo();
    state.layers[0].locked = true;
    Layer grade;
    grade.name = L"Warm grade";
    grade.blend = Blend::multiply;
    grade.opacity = 94;
    gradient(grade, state.bounds(), {0, 0}, {state.width, state.height},
        rgba(202, 183, 162, 130), rgba(241, 208, 157, 10));
    Layer lettering;
    lettering.name = L"Field Notes";
    const auto cream = rgba(255, 245, 222);
    const auto bounds = state.bounds();
    drawType(lettering, bounds, {108, 292}, L"A T L A N T I C   C O A S T   /   0 1", 26, cream, L"Segoe UI", FW_SEMIBOLD);
    drawType(lettering, bounds, {101, 327}, L"Edge of\n the Atlantic", 114, cream, L"Georgia");
    fill(lettering, {111, 606, 100, 2}, cream);
    drawType(lettering, bounds, {108, 624}, L"C O A S T A L   F I E L D   N O T E S", 22, cream, L"Segoe UI", FW_SEMIBOLD);
    Layer retouch;
    retouch.name = L"Retouch";
    state.layers.push_back(std::move(grade));
    state.layers.push_back(std::move(lettering));
    state.layers.push_back(std::move(retouch));
    state.active = 3;
    addTab(std::move(state), L"Atlantic Coast.pxf", L"Open Sample");
}
Vec App::mouse(LPARAM p) const { return {GET_X_LPARAM(p) / dpi_, GET_Y_LPARAM(p) / dpi_}; }
Vec App::imageToScreen(float x, float y) const {
    const auto t = tab();
    return {(viewport_.left + viewport_.right) / 2 + t->pan.x + (x - t->doc.state().width / 2.0f) * t->zoom,
        (viewport_.top + viewport_.bottom) / 2 + t->pan.y + (y - t->doc.state().height / 2.0f) * t->zoom};
}
Vec App::screenToImage(Vec p) const {
    const auto origin = imageToScreen(0, 0);
    return {(p.x - origin.x) / tab()->zoom, (p.y - origin.y) / tab()->zoom};
}
Point App::imagePoint(Vec p) const {
    auto v = screenToImage(p);
    return {int(std::floor(v.x)), int(std::floor(v.y))};
}
Box App::imageRect() const {
    const auto start = imageToScreen(0, 0);
    const auto end = imageToScreen(float(tab()->doc.state().width), float(tab()->doc.state().height));
    return {start.x, start.y, end.x, end.y};
}
Rect App::clip() const {
    const auto& d = tab()->doc;
    return d.selection ? intersect(d.selection->bounds(), d.state().bounds()) : d.state().bounds();
}
void App::setSelection(Selection selected, std::wstring label) {
    auto t = tab();
    if (!t) return;
    t->doc.begin();
    if (selected.empty()) {
        if (t->doc.selection) t->lastSelection = std::move(t->doc.selection);
        t->doc.selection.reset();
    } else t->doc.selection = std::move(selected);
    t->doc.commit(std::move(label));
    if (t->doc.historyStartId() != compareId_) { beforeBitmaps_.clear(); compareId_ = t->doc.historyStartId(); }
    repaint();
}
SelectMode App::selectionModeAtPress() const {
    if (shift()) return alt() ? SelectMode::intersect : SelectMode::add;
    return alt() ? SelectMode::subtract : selectionMode_;
}
void App::finishLasso() {
    const auto t = tab();
    if (t && lassoPath_.size() >= 3) {
        const auto region = lassoSelection(lassoPath_, t->doc.state().bounds());
        setSelection(combineSelection(t->doc.selection.value_or(Selection{}), region, gestureMode_), L"Lasso");
    }
    lassoPath_.clear();
    repaint();
}
void App::fit() {
    if (auto t = tab()) { t->fit = true; t->pan = {}; repaint(); }
}
void App::zoomAt(float value, Vec at) {
    auto t = tab();
    if (!t) return;
    const auto point = screenToImage(at);
    t->zoom = std::clamp(value, 0.0125f, 32.0f);
    t->fit = false;
    auto after = imageToScreen(point.x, point.y);
    t->pan.x += at.x - after.x;
    t->pan.y += at.y - after.y;
    repaint();
}
void App::setTool(Tool tool) {
    if (tool_ == Tool::crop && tab() && tool != tool_) tab()->cropBox.reset();
    if (tool != tool_) { resetTransform(); lassoPath_.clear(); }
    tool_ = tool;
    const auto info = std::find_if(std::begin(tools), std::end(tools), [&](const ToolInfo& item) { return item.tool == tool; });
    if (info != std::end(tools)) preferredTools_[info->key - L'A'] = tool;
    repaint();
}
void App::chooseColor(bool foreground) {
    static COLORREF colors[16]{};
    auto& value = foreground ? foreground_ : background_;
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice); choice.hwndOwner = window_; choice.lpCustColors = colors;
    choice.rgbResult = RGB(red(value), green(value), blue(value));
    choice.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&choice)) {
        value = rgba(GetRValue(choice.rgbResult), GetGValue(choice.rgbResult), GetBValue(choice.rgbResult));
        updateHSV();
        repaint();
    }
}
void App::updateHSV() {
    const float r = red(foreground_) / 255.0f, g = green(foreground_) / 255.0f, b = blue(foreground_) / 255.0f;
    const float max = std::max({r, g, b}), min = std::min({r, g, b}), delta = max - min;
    val_ = max; sat_ = max > 0 ? delta / max : 0;
    if (delta > 0) {
        hueValue_ = (max == r ? (g - b) / delta : max == g ? 2 + (b - r) / delta : 4 + (r - g) / delta) / 6;
        if (hueValue_ < 0) hueValue_ += 1;
        spectrum_.reset();
    }
}
void App::changeHSV(float h, float s, float v) {
    h = std::clamp(h, 0.0f, 1.0f);
    if (hueValue_ != h) spectrum_.reset();
    hueValue_ = h; sat_ = std::clamp(s, 0.0f, 1.0f); val_ = std::clamp(v, 0.0f, 1.0f);
    auto channel = [&](float offset) {
        const float k = std::fmod(offset + h * 6, 6.0f);
        return int(255 * (val_ - val_ * sat_ * std::max(0.0f, std::min({k, 4 - k, 1.0f}))) + 0.5f);
    };
    foreground_ = rgba(channel(5), channel(3), channel(1));
    repaint();
}
void App::number(const wchar_t* label, int& value, int low, int high) {
    const Field field{std::wstring(label) + L" (" + std::to_wstring(low) + L"–" + std::to_wstring(high) + L")", std::to_wstring(value)};
    if (auto input = prompt(window_, label, std::span(&field, 1))) value = readNumber(input->front(), low, high);
    repaint();
}
void App::command(Command c) {
    if (drag_ != Drag::none) cancelDrag();
    auto t = tab();
    if (c == Command::none) return;
    if (c == Command::quit) { SendMessageW(window_, WM_CLOSE, 0, 0); return; }
    if (c == Command::open || c == Command::place) {
        if (auto path = fileDialog(window_, false, false); !path.empty()) {
            if (t && c == Command::place) resetTransform();
            openPath(path, c == Command::place);
        }
        return;
    }
    if (c == Command::demo) { openDemo(); return; }
    if (c == Command::newDoc) { createDocument(); return; }
    if (c == Command::about) {
        MessageBoxW(window_, L"PixelForge 1.2\n\nA native raster editor for Windows.\nC++20 · Direct2D · Windows Imaging Component",
            L"About PixelForge", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (c == Command::shortcuts) {
        std::wstring help = L"Tools\n";
        for (const auto& tool : tools) help += std::wstring(1, tool.key) + L"  " + tool.name + L"\n";
        help += L"\nShift + tool key cycles tools in a group.\nShift / Alt / Shift+Alt add, subtract or intersect selections."
            L"\nCtrl+J / Ctrl+Shift+J copy or cut the selection to a layer.\nCtrl+Shift+I inverts. Ctrl+Shift+D reselects."
            L"\n[ / ] adjust brush size. Shift+[ / Shift+] adjust hardness.";
        MessageBoxW(window_, help.c_str(), L"Keyboard Shortcuts", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (c == Command::panels) { showPanels_ = !showPanels_; repaint(); return; }
    if (c == Command::rightPanels) { showRightPanels_ = !showRightPanels_; repaint(); return; }
    if (c == Command::rulers) { showRulers_ = !showRulers_; repaint(); return; }
    if (c == Command::extras) { showExtras_ = !showExtras_; repaint(); return; }
    if (c == Command::guides) { showGuides_ = !showGuides_; repaint(); return; }
    if (c == Command::lockGuides) { guidesLocked_ = !guidesLocked_; repaint(); return; }
    if (c == Command::snap) { snap_ = !snap_; repaint(); return; }
    if (c == Command::contextBar) { showContextBar_ = !showContextBar_; repaint(); return; }
    if (!t) return;
    if (c == Command::screenStandard || c == Command::screenMenus || c == Command::screenFull) {
        setScreenMode(int(c) - int(Command::screenStandard));
        return;
    }
    if (t->transform && c != Command::applyTransform && c != Command::cancelTransform &&
        c != Command::fit && c != Command::actual && c != Command::zoomIn && c != Command::zoomOut) {
        resetTransform();
    }
    auto& doc = t->doc;
    auto& state = doc.edit();
    auto& l = doc.active();
    auto commit = [&](const wchar_t* label) {
        doc.commit(label);
        documentChanged();
    };
    auto editable = [&] {
        if (l.locked) { MessageBeep(MB_ICONWARNING); return false; }
        return true;
    };
    const Vec center{(viewport_.left + viewport_.right) / 2, (viewport_.top + viewport_.bottom) / 2};
    switch (c) {
    case Command::save: case Command::saveAs: saveTab(*t, c == Command::saveAs); return;
    case Command::exportFile:
        if (auto path = fileDialog(window_, true, true, std::filesystem::path(t->title).stem().wstring()); !path.empty())
            exportImage(state, path);
        return;
    case Command::close: closeTab(current_); return;
    case Command::undo: if (doc.undo()) documentChanged(); return;
    case Command::redo: if (doc.redo()) documentChanged(); return;
    case Command::cut: if (editable() && (!l.adjustment || t->mask)) copy(true); return;
    case Command::copy: copy(false); return;
    case Command::copyMerged: copy(false, true); return;
    case Command::paste: paste(); return;
    case Command::fit: fit(); return;
    case Command::actual: zoomAt(1, center); return;
    case Command::zoomIn: zoomAt(t->zoom * 1.25f, center); return;
    case Command::zoomOut: zoomAt(t->zoom / 1.25f, center); return;
    case Command::compare:
        t->compare = !t->compare; beforeBitmaps_.clear(); repaint(); return;
    case Command::freeTransform: beginTransform(); return;
    case Command::applyTransform: applyTransform(); return;
    case Command::cancelTransform: resetTransform(); return;
    case Command::addTone: adjustment(AdjustmentKind::tone); return;
    case Command::addLevels: adjustment(AdjustmentKind::levels); return;
    case Command::addHue: adjustment(AdjustmentKind::hueSaturation); return;
    case Command::editAdjustment: adjustment(std::nullopt); return;
    case Command::strokeStyle: case Command::overlayStyle: case Command::shadowStyle: case Command::clearStyle:
        layerStyle(c); return;
    case Command::mergeDown:
        doc.begin();
        if (mergeDown(state)) { t->mask = false; commit(L"Merge Down"); }
        else {
            doc.cancel();
            MessageBoxW(window_, L"Merge Down requires two visible raster layers in Normal mode.", L"Merge Down", MB_OK | MB_ICONINFORMATION);
        }
        return;
    case Command::mergeVisible: case Command::flatten:
        doc.begin(); mergeVisible(state, c == Command::flatten);
        t->mask = false; commit(c == Command::flatten ? L"Flatten Image" : L"Merge Visible"); return;
    case Command::resolution: {
        int ppi = int(std::round(state.ppi));
        const int before = ppi;
        number(L"Resolution (PPI)", ppi, 1, 2400);
        if (ppi != before) { doc.begin(); state.ppi = ppi; commit(L"Resolution"); }
        return;
    }
    case Command::selectAll: setSelection(state.bounds(), L"Select All"); return;
    case Command::deselect: setSelection({}, L"Deselect"); t->cropBox.reset(); lassoPath_.clear(); repaint(); return;
    case Command::inverseSelection:
        if (doc.selection) setSelection(combineSelection(Selection(state.bounds()), *doc.selection, SelectMode::subtract), L"Inverse");
        return;
    case Command::reselect:
        if (!doc.selection && t->lastSelection)
            setSelection(combineSelection(*t->lastSelection, Selection(state.bounds()), SelectMode::intersect));
        return;
    case Command::loadSelection: setSelection(selectLayerPixels(l, state.bounds())); return;
    case Command::newGuide: {
        if (guidesLocked_) return;
        if (state.guides.size() >= maxGuides) throw std::runtime_error("This document has reached its guide limit.");
        const std::array fields{Field{L"Orientation (H or V)", L"V"}, Field{L"Position (px)", L"0"}};
        if (const auto values = prompt(window_, L"New Guide", fields)) {
            const auto& axis = (*values)[0];
            const bool vertical = axis == L"V" || axis == L"v";
            if (!vertical && axis != L"H" && axis != L"h") throw std::runtime_error("Enter H for horizontal or V for vertical.");
            const Guide guide{vertical, readNumber((*values)[1], 0, vertical ? state.width : state.height)};
            if (std::find(state.guides.begin(), state.guides.end(), guide) == state.guides.end()) {
                doc.begin(); state.guides.push_back(guide); doc.commit(L"New Guide");
            }
            showGuides_ = showExtras_ = true;
            documentChanged(false);
        }
        return;
    }
    case Command::clearGuides:
        if (guidesLocked_ || state.guides.empty()) return;
        doc.begin(); state.guides.clear(); doc.commit(L"Clear Guides"); documentChanged(false); return;
    case Command::cropSelection: {
        const auto area = t->cropBox ? t->cropBox : doc.selection ? std::optional(doc.selection->bounds()) : std::nullopt;
        if (!area || area->empty()) return;
        doc.begin(); crop(state, *area);
        t->cropBox.reset(); doc.selection.reset();
        fit(); commit(L"Crop"); return;
    }
    case Command::resize: case Command::canvasSize: {
        const std::array fields{Field{L"Width (px)", std::to_wstring(state.width)}, Field{L"Height (px)", std::to_wstring(state.height)}};
        if (auto input = prompt(window_, c == Command::resize ? L"Image Size" : L"Canvas Size", fields)) {
            const int w = readNumber((*input)[0], 1, maxEdge), h = readNumber((*input)[1], 1, maxEdge);
            if (w == state.width && h == state.height) return;
            doc.begin();
            if (c == Command::resize) state = resize(state, w, h);
            else resizeCanvas(state, w, h);
            doc.selection.reset(); t->cropBox.reset();
            fit(); commit(c == Command::resize ? L"Image Size" : L"Canvas Size");
        }
        return;
    }
    case Command::flipX: case Command::flipY:
        doc.begin(); flip(state, c == Command::flipX); doc.selection.reset(); t->cropBox.reset();
        commit(c == Command::flipX ? L"Flip Horizontal" : L"Flip Vertical"); return;
    case Command::rotate:
        doc.begin(); rotateClockwise(state); doc.selection.reset(); t->cropBox.reset();
        fit(); commit(L"Rotate Canvas"); return;
    case Command::blur: {
        if (!editable() || l.adjustment) return;
        int radius = 0;
        const Field field{L"Radius (1–64 px)", L"5"};
        if (auto input = prompt(window_, L"Box Blur", std::span(&field, 1))) radius = readNumber(input->front(), 1, 64);
        if (radius) { doc.begin(); boxBlur(l, clip(), radius, selection()); commit(L"Box Blur"); }
        return;
    }
    case Command::clear: case Command::fillFG: case Command::fillBG:
        if (!editable() || (l.adjustment && !t->mask)) return;
        doc.begin();
        fill(l, clip(), c == Command::fillBG ? background_ : c == Command::clear ? 0xffffffff : foreground_,
            c == Command::clear, t->mask, selection());
        commit(c == Command::clear ? L"Clear" : L"Fill"); return;
    case Command::invert: case Command::gray: case Command::brightness: case Command::contrast: {
        if (!editable() || l.adjustment) return;
        const auto filter = c == Command::invert ? Filter::invert : c == Command::gray ? Filter::grayscale :
            c == Command::brightness ? Filter::brightness : Filter::contrast;
        const auto label = c == Command::invert ? L"Invert" : c == Command::gray ? L"Desaturate" :
            c == Command::brightness ? L"Brightness" : L"Contrast";
        int amount = 0;
        if (c == Command::brightness || c == Command::contrast) {
            const Field field{std::wstring(label) + L" (-100 to 100)", L"0"};
            const auto input = prompt(window_, label, std::span(&field, 1));
            if (!input) return;
            amount = readNumber(input->front(), -100, 100);
            if (!amount) return;
        }
        doc.begin(); adjust(l, clip(), filter, amount, selection()); commit(label); return;
    }
    case Command::addLayer: case Command::newLayerDialog: case Command::duplicate: case Command::layerViaCut: {
        if (state.layers.size() >= maxLayers) throw std::runtime_error("This document has reached its layer limit.");
        const bool fromExisting = c == Command::duplicate || c == Command::layerViaCut;
        const bool cut = c == Command::layerViaCut;
        if ((cut && (!selection() || !editable() || l.adjustment || t->mask)) ||
            (fromExisting && selection() && l.adjustment)) return;
        Layer layer = fromExisting ? (selection() ? layerViaSelection(l, *selection()) : l) : Layer{};
        layer.name = fromExisting ? layer.name + L" copy" : L"Layer " + std::to_wstring(state.layers.size() + 1);
        if (fromExisting) layer.locked = false;
        if (c == Command::newLayerDialog) {
            const Field field{L"Name", layer.name};
            const auto input = prompt(window_, L"New Layer", std::span(&field, 1));
            if (!input) return;
            if (!input->front().empty()) layer.name = input->front();
        }
        doc.begin();
        if (cut) fill(l, clip(), 0xffffffff, true, false, selection());
        state.layers.insert(state.layers.begin() + ++state.active, std::move(layer));
        t->mask = false; t->layerScroll = 0;
        commit(cut ? L"Layer via Cut" : c == Command::duplicate ? L"Layer via Copy" : L"New Layer"); return;
    }
    case Command::rename: {
        const Field field{L"Name", l.name};
        if (auto input = prompt(window_, L"Layer Name", std::span(&field, 1)); input && !input->front().empty()) {
            doc.begin(); l.name = input->front(); commit(L"Rename Layer");
        }
        return;
    }
    case Command::deleteLayer: {
        if (!editable()) return;
        doc.begin(); state.layers.erase(state.layers.begin() + state.active);
        if (state.layers.empty()) state.layers.emplace_back();
        state.active = std::min(state.active, int(state.layers.size()) - 1);
        t->mask = false;
        commit(L"Delete Layer"); return;
    }
    case Command::layerUp: case Command::layerDown: {
        const int next = state.active + (c == Command::layerUp ? 1 : -1);
        if (next < 0 || next >= int(state.layers.size())) return;
        doc.begin(); std::swap(state.layers[next], state.layers[state.active]); state.active = next;
        commit(L"Reorder Layers"); return;
    }
    case Command::toggleLock: doc.begin(); l.locked = !l.locked; commit(L"Toggle Lock"); return;
    case Command::addMask: {
        if (!editable() || l.mask) return;
        const Rect bounds{-l.x, -l.y, state.width, state.height};
        doc.begin(); l.mask = maskFromSelection(bounds, doc.selection, {l.x, l.y}); l.maskEnabled = true;
        t->mask = true; commit(L"Add Layer Mask"); return;
    }
    case Command::removeMask:
        if (!editable() || !l.mask) return;
        doc.begin(); l.mask.reset(); l.maskEnabled = true; t->mask = false; commit(L"Delete Layer Mask"); return;
    default: return;
    }
}
void App::menu(int index, Vec point, bool context) {
    if (index < 0 || index >= int(std::size(menus))) return;
    auto popup = CreatePopupMenu();
    for (const auto& item : menus[index]) {
        if (item.command == Command::none) { AppendMenuW(popup, MF_SEPARATOR, 0, nullptr); continue; }
        const bool noDocument = !tab() && item.command != Command::newDoc && item.command != Command::open &&
            item.command != Command::demo && item.command != Command::about && item.command != Command::quit &&
            item.command != Command::panels && item.command != Command::rulers && item.command != Command::shortcuts;
        const bool disabled = noDocument || (item.command == Command::undo && !tab()->doc.historyIndex()) ||
            (item.command == Command::redo && tab()->doc.historyIndex() + 1 == tab()->doc.historyCount()) ||
            (item.command == Command::paste && !IsClipboardFormatAvailable(CF_DIBV5) && !IsClipboardFormatAvailable(CF_DIB));
        const bool checked = (item.command == Command::rulers && showRulers_) || (item.command == Command::panels && showPanels_) ||
            (item.command == Command::contextBar && showContextBar_) || (item.command == Command::compare && tab() && tab()->compare) ||
            (item.command == Command::extras && showExtras_) || (item.command == Command::snap && snap_) ||
            (item.command == Command::guides && showGuides_) || (item.command == Command::lockGuides && guidesLocked_) ||
            (item.command == Command::rightPanels && showRightPanels_);
        AppendMenuW(popup, MF_STRING | (disabled ? MF_GRAYED : 0) | (checked ? MF_CHECKED : 0),
            UINT_PTR(item.command), item.label);
    }
    if (index == 3) {
        const auto child = CreatePopupMenu();
        for (const auto& item : adjustmentsMenu) AppendMenuW(child, MF_STRING, UINT_PTR(item.command), item.label);
        InsertMenuW(popup, 1, MF_BYPOSITION | MF_POPUP | (tab() ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(child), L"New Adjustment Layer");
        const auto fx = CreatePopupMenu();
        for (const auto& item : stylesMenu) AppendMenuW(fx, MF_STRING, UINT_PTR(item.command), item.label);
        InsertMenuW(popup, 2, MF_BYPOSITION | MF_POPUP | (tab() ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(fx), L"Layer Style");
    }
    if (index == 6) {
        const auto screen = CreatePopupMenu();
        for (const auto& item : screenMenu) AppendMenuW(screen, MF_STRING |
            ((int(item.command) - int(Command::screenStandard) == screenMode_) ? MF_CHECKED : 0), UINT_PTR(item.command), item.label);
        AppendMenuW(popup, MF_POPUP | (tab() ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(screen), L"Screen Mode");
    }
    POINT screen{LONG(point.x * dpi_), LONG(point.y * dpi_)};
    ClientToScreen(window_, &screen);
    const auto choice = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(popup);
    if (choice) command(Command(choice));
    if (context) repaint();
}
void App::popupBlend(Vec at) {
    if (!tab()) return;
    auto& doc = tab()->doc;
    auto popup = CreatePopupMenu();
    for (const auto& item : blendOptions) {
        if (item.separator) AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(popup, MF_STRING | (doc.active().blend == item.mode ? MF_CHECKED : 0), UINT_PTR(item.mode) + 1, item.name);
    }
    POINT screen{LONG(at.x * dpi_), LONG(at.y * dpi_)};
    ClientToScreen(window_, &screen);
    const auto choice = TrackPopupMenu(popup, TPM_RETURNCMD, screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(popup);
    if (!choice) return;
    doc.begin(); doc.active().blend = Blend(choice - 1); doc.commit(L"Blend Mode");
    documentChanged();
}
void App::popupStyles(Vec at) {
    if (!tab()) return;
    auto popup = CreatePopupMenu();
    const auto& layer = tab()->doc.active();
    for (const auto& item : stylesMenu) {
        const bool on = (item.command == Command::strokeStyle && alpha(layer.style.stroke)) ||
            (item.command == Command::overlayStyle && alpha(layer.style.overlay)) ||
            (item.command == Command::shadowStyle && alpha(layer.style.shadow));
        const bool unavailable = layer.adjustment || layer.locked || (item.command == Command::clearStyle && layer.style.empty());
        AppendMenuW(popup, MF_STRING | (on ? MF_CHECKED : 0) | (unavailable ? MF_GRAYED : 0), UINT_PTR(item.command), item.label);
    }
    POINT screen{LONG(at.x * dpi_), LONG(at.y * dpi_)};
    ClientToScreen(window_, &screen);
    const auto choice = TrackPopupMenu(popup, TPM_RETURNCMD, screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(popup);
    if (choice) command(Command(choice));
}
void App::popupTools(Vec at, wchar_t group) {
    auto popup = CreatePopupMenu();
    for (const auto& tool : tools) {
        if (group && tool.key != group) continue;
        std::wstring label = std::wstring(tool.name) + L"\t" + tool.key;
        AppendMenuW(popup, MF_STRING | (tool.tool == tool_ ? MF_CHECKED : 0), UINT_PTR(tool.tool) + 1, label.c_str());
    }
    POINT screen{LONG(at.x * dpi_), LONG(at.y * dpi_)};
    ClientToScreen(window_, &screen);
    const auto choice = TrackPopupMenu(popup, TPM_RETURNCMD, screen.x, screen.y, 0, window_, nullptr);
    DestroyMenu(popup);
    if (choice) setTool(Tool(choice - 1));
}
void App::inputTarget(const Target& hit, Vec p) {
    auto t = tab();
    switch (hit.kind) {
    case Hit::menu: menu(hit.id, {hit.rect.left, hit.rect.bottom}); break;
    case Hit::tab: switchTab(hit.id); break;
    case Hit::closeTab: closeTab(hit.id); break;
    case Hit::tool: setTool(Tool(hit.id)); break;
    case Hit::color: chooseColor(hit.id == 0); break;
    case Hit::swap: std::swap(foreground_, background_); updateHSV(); repaint(); break;
    case Hit::defaults: foreground_ = rgba(0, 0, 0); background_ = rgba(255, 255, 255); updateHSV(); repaint(); break;
    case Hit::fgSize: number(L"Brush Size", brushSize_, 1, 2000); break;
    case Hit::hardness: number(L"Hardness (%)", hardness_, 0, 100); break;
    case Hit::flow: number(L"Opacity (%)", flow_, 1, 100); break;
    case Hit::typeSize: number(L"Type Size (px)", typeSize_, 6, 800); break;
    case Hit::tolerance: number(L"Tolerance", hit.id ? wandTolerance_ : tolerance_, 0, 255); break;
    case Hit::selectionMode: selectionMode_ = SelectMode(hit.id); repaint(); break;
    case Hit::wandContiguous: wandContiguous_ = !wandContiguous_; repaint(); break;
    case Hit::wandMerged: wandMerged_ = !wandMerged_; repaint(); break;
    case Hit::autoSelect: autoSelect_ = !autoSelect_; repaint(); break;
    case Hit::ruler: startGuide(hit.id != 0, std::nullopt, p); break;
    case Hit::cloneSample: cloneMerged_ = !cloneMerged_; repaint(); break;
    case Hit::transformSize: if (t && t->transform) transformNumber(false); break;
    case Hit::transformAngle: if (t && t->transform) transformNumber(true); break;
    case Hit::transformLink: if (t && t->transform) { t->transform->linked = !t->transform->linked; repaint(); } break;
    case Hit::properties: colorTab_ = 2; repaint(); break;
    case Hit::compareSlider:
        drag_ = Drag::compare;
        SetCapture(window_); moveMouse(p); break;
    case Hit::fit: fit(); break;
    case Hit::undo: command(Command::undo); break;
    case Hit::redo: command(Command::redo); break;
    case Hit::blend: popupBlend({hit.rect.left, hit.rect.bottom}); break;
    case Hit::styles:
        if (hit.id >= 0 && t) { t->doc.select(hit.id); t->mask = false; resetTransform(); }
        popupStyles({hit.rect.left, hit.rect.bottom}); repaint(); break;
    case Hit::opacity: case Hit::fillOpacity: {
        auto& doc = t->doc;
        const bool fillOnly = hit.kind == Hit::fillOpacity;
        if (fillOnly && doc.active().adjustment) return;
        auto& value = fillOnly ? doc.active().fillOpacity : doc.active().opacity;
        int opacity = (value * 100 + 127) / 255;
        const int before = opacity;
        number(fillOnly ? L"Fill Opacity (%)" : L"Layer Opacity (%)", opacity, 0, 100);
        if (opacity != before) {
            doc.begin(); value = (opacity * 255 + 50) / 100; doc.commit(fillOnly ? L"Fill Opacity" : L"Layer Opacity");
            documentChanged();
        }
        break;
    }
    case Hit::layerThumbnail:
        if (control()) {
            auto mode = selectionModeAtPress();
            const auto region = selectLayerPixels(t->doc.state().layers[hit.id], t->doc.state().bounds());
            setSelection(combineSelection(t->doc.selection.value_or(Selection{}), region, mode));
            break;
        }
        [[fallthrough]];
    case Hit::layer:
        t->doc.select(hit.id); t->mask = false; resetTransform();
        drag_ = Drag::layerRow; draggedLayer_ = dropLayer_ = hit.id; down_ = p;
        SetCapture(window_); repaint(); break;
    case Hit::eye: case Hit::lock: {
        auto& doc = t->doc;
        if (hit.kind == Hit::lock && hit.id == doc.state().active) resetTransform();
        doc.begin();
        auto& layer = doc.edit().layers[hit.id];
        if (hit.kind == Hit::eye) layer.visible = !layer.visible;
        else layer.locked = !layer.locked;
        doc.commit(hit.kind == Hit::eye ? L"Layer Visibility" : L"Toggle Lock");
        documentChanged();
        break;
    }
    case Hit::mask:
        t->doc.select(hit.id); t->mask = true; resetTransform();
        if (shift()) {
            auto& doc = t->doc;
            doc.begin(); doc.active().maskEnabled = !doc.active().maskEnabled; doc.commit(L"Toggle Layer Mask");
            documentChanged();
        } else repaint();
        break;
    case Hit::history: if (t->doc.jump(hit.id)) { t->mask = false; resetTransform(); documentChanged(); } break;
    case Hit::panel: sidebarTab_ = hit.id; repaint(); break;
    case Hit::colorTab: colorTab_ = hit.id; repaint(); break;
    case Hit::saturation: case Hit::hue:
        drag_ = hit.kind == Hit::hue ? Drag::colorHue : Drag::colorSV;
        SetCapture(window_); moveMouse(p); break;
    case Hit::swatch: foreground_ = 0xff000000u | Pixel(hit.id); updateHSV(); repaint(); break;
    case Hit::command: command(Command(hit.id)); break;
    case Hit::zoom: {
        if (hit.id) { command(hit.id < 0 ? Command::zoomOut : Command::zoomIn); break; }
        int percent = int(t->zoom * 100 + 0.5f);
        number(L"Zoom (%)", percent, 1, 3200);
        zoomAt(percent / 100.0f, {(viewport_.left + viewport_.right) / 2, (viewport_.top + viewport_.bottom) / 2});
        break;
    }
    case Hit::applyCrop: command(Command::cropSelection); break;
    case Hit::cancelCrop: if (t) t->cropBox.reset(); repaint(); break;
    case Hit::moreTools: popupTools({hit.rect.right, hit.rect.top}, wchar_t(hit.id)); break;
    case Hit::zoomMode: zoomOutMode_ = hit.id != 0; repaint(); break;
    }
}

void App::leftDown(Vec p, bool middle) {
    cursor_ = p;
    SetFocus(window_);
    if (drag_ != Drag::none) return;
    KillTimer(window_, 1); tooltip_ = false;
    if (!middle) for (auto i = hits_.rbegin(); i != hits_.rend(); ++i) {
        if (inside(i->rect, p)) { auto hit = *i; inputTarget(hit, p); return; }
    }
    auto t = tab();
    if (!t || !inside(viewport_, p)) return;
    auto& doc = t->doc;
    down_ = last_ = p;
    anchor_ = imagePoint(p);
    const bool onImage = doc.state().bounds().contains(anchor_);
    if (middle || space_ || (tool_ == Tool::hand && !t->transform)) {
        drag_ = Drag::pan; panStart_ = t->pan;
    } else if (t->transform) {
        if (!startTransformDrag(p)) return;
    } else if (const auto guide = tool_ == Tool::move && !control() ? nearestGuide(p) : std::nullopt) {
        startGuide(doc.state().guides[*guide].vertical, guide, p);
        return;
    } else if (tool_ == Tool::zoom) {
        zoomAt(t->zoom * ((alt() || zoomOutMode_) ? 0.8f : 1.25f), p); return;
    } else if (!onImage) {
        if (selectionTool(tool_) && lassoPath_.empty() && selectionModeAtPress() == SelectMode::replace) command(Command::deselect);
        return;
    } else if (tool_ == Tool::clone && alt()) {
        t->cloneSource = anchor_;
        repaint();
        return;
    } else if (tool_ == Tool::picker || (tool_ == Tool::brush && alt())) {
        std::array<Pixel, 1> color{};
        composite(doc.state(), {anchor_.x, anchor_.y, 1, 1}, color, 1);
        if (alpha(color[0])) foreground_ = 0xff000000u | (straight(color[0]) & 0xffffffu);
        updateHSV(); repaint(); return;
    } else if (tool_ == Tool::wand) {
        const auto area = wandSelection(doc.state(), anchor_, wandTolerance_, wandContiguous_, wandMerged_);
        setSelection(combineSelection(doc.selection.value_or(Selection{}), area, selectionModeAtPress()), L"Magic Wand");
        return;
    } else if (tool_ == Tool::polygonLasso) {
        if (lassoPath_.empty()) gestureMode_ = selectionModeAtPress();
        else if (lassoPath_.size() >= 3) {
            const auto first = imageToScreen(float(lassoPath_.front().x), float(lassoPath_.front().y));
            if (std::hypot(p.x - first.x, p.y - first.y) <= 7.0f) { finishLasso(); return; }
        }
        if (lassoPath_.size() < 32768 && (lassoPath_.empty() || lassoPath_.back() != anchor_)) lassoPath_.push_back(anchor_);
        repaint();
        return;
    } else if (tool_ == Tool::lasso) {
        drag_ = Drag::lasso;
        gestureMode_ = selectionModeAtPress();
        lassoPath_ = {anchor_};
    } else if (tool_ == Tool::marquee || tool_ == Tool::ellipticalMarquee || tool_ == Tool::crop) {
        drag_ = tool_ == Tool::crop ? Drag::crop : Drag::marquee;
        gestureMode_ = selectionModeAtPress();
        gesture_ = {anchor_.x, anchor_.y, 1, 1};
    } else if (tool_ == Tool::type) {
        if (doc.state().layers.size() >= maxLayers) throw std::runtime_error("This document has reached its layer limit.");
        const Field field{L"Text", L"", true};
        if (auto input = prompt(window_, L"Add Text", std::span(&field, 1)); input && !input->front().empty()) {
            Layer layer;
            const auto end = input->front().find_first_of(L"\r\n");
            layer.name = input->front().substr(0, std::min<std::size_t>(end, 48));
            if (layer.name.empty()) layer.name = L"Text";
            drawType(layer, doc.state().bounds(), anchor_, input->front(), typeSize_, foreground_);
            doc.begin();
            auto& s = doc.edit();
            s.layers.insert(s.layers.begin() + ++s.active, std::move(layer));
            doc.commit(L"Add Text"); t->mask = false; documentChanged();
        }
        return;
    } else {
        if (tool_ == Tool::move && (autoSelect_ != control())) {
            const auto& layers = doc.state().layers;
            for (int i = int(layers.size()) - 1; i >= 0; --i) {
                const auto& layer = layers[i];
                const int x = anchor_.x - layer.x, y = anchor_.y - layer.y;
                if (layer.visible && layer.opacity && !layer.adjustment && alpha(layer.pixels.pixel(x, y)) &&
                    (!layer.mask || !layer.maskEnabled || red(layer.mask->pixel(x, y)))) { doc.select(i); t->mask = false; break; }
            }
        }
        if (doc.active().locked || (doc.active().adjustment &&
            (!t->mask || (tool_ != Tool::brush && tool_ != Tool::eraser && tool_ != Tool::bucket && tool_ != Tool::gradient))) ||
            (tool_ == Tool::clone && (!t->cloneSource || t->mask))) { MessageBeep(MB_ICONWARNING); return; }
        doc.begin();
        if (tool_ == Tool::brush || tool_ == Tool::eraser || tool_ == Tool::clone) {
            drag_ = Drag::paint;
            strokeCarry_ = 0;
            const auto v = screenToImage(p);
            lastStamp_ = {v.x, v.y};
            if (tool_ == Tool::clone) cloneOffset_ = {t->cloneSource->x - anchor_.x, t->cloneSource->y - anchor_.y};
            Rect dirty;
            dab(v.x, v.y, dirty);
            invalidate(dirty);
        } else if (tool_ == Tool::move) {
            drag_ = Drag::move; moveStart_ = {doc.active().x, doc.active().y};
            moveBounds_ = snap_ ? doc.active().pixels.contentBounds() : Rect{};
            moveBounds_.x += moveStart_.x; moveBounds_.y += moveStart_.y;
        } else if (tool_ == Tool::bucket) {
            flood(doc.active(), clip(), anchor_, foreground_, tolerance_, t->mask, selection());
            doc.commit(L"Paint Bucket"); documentChanged();
            return;
        } else if (tool_ == Tool::gradient) drag_ = Drag::gradient;
        else { drag_ = Drag::shape; gesture_ = {anchor_.x, anchor_.y, 1, 1}; }
    }
    SetCapture(window_);
    repaint();
}
void App::stroke(Vec p) {
    const auto from = screenToImage(last_);
    const auto to = screenToImage(p);
    const float distance = std::hypot(to.x - from.x, to.y - from.y);
    const float spacing = std::max(0.5f, brushSize_ * 0.14f);
    Rect dirty;
    if (distance > 0) {
        float at = spacing - strokeCarry_;
        for (; at <= distance; at += spacing) {
            const float t = at / distance;
            lastStamp_ = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
            dab(lastStamp_.x, lastStamp_.y, dirty);
        }
        strokeCarry_ = distance - (at - spacing);
    }
    invalidate(dirty);
}
void App::moveMouse(Vec p) {
    cursor_ = p;
    tooltip_ = false;
    if (!overWindow_) {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
        TrackMouseEvent(&tracking);
        overWindow_ = true;
    }
    if (drag_ == Drag::none) SetTimer(window_, 1, 620, nullptr);
    const auto cursor = drag_ == Drag::guide ? (guideDraft_.vertical ? IDC_SIZEWE : IDC_SIZENS) :
        drag_ == Drag::compare ? IDC_SIZEWE : drag_ == Drag::transform ? IDC_SIZEALL :
        drag_ == Drag::pan || (inside(viewport_, p) && (space_ || tool_ == Tool::hand)) ? IDC_HAND :
        inside(viewport_, p) && tab() && tool_ != Tool::move && tool_ != Tool::brush && tool_ != Tool::eraser ?
        (tool_ == Tool::type ? IDC_IBEAM : IDC_CROSS) : IDC_ARROW;
    SetCursor(LoadCursorW(nullptr, cursor));
    auto t = tab();
    if (!t) { repaint(); return; }
    if (drag_ == Drag::pan) {
        t->fit = false;
        t->pan = {panStart_.x + p.x - down_.x, panStart_.y + p.y - down_.y};
    } else if (drag_ == Drag::paint) {
        stroke(p);
    } else if (drag_ == Drag::guide) {
        const auto position = screenToImage(p);
        guideDraft_.position = int(std::round(guideDraft_.vertical ? position.x : position.y));
    } else if (drag_ == Drag::transform && t->transform) {
        dragTransform(p);
        bitmaps_.clear();
    } else if (drag_ == Drag::compare) {
        const auto image = imageRect();
        const float left = std::max(image.left, viewport_.left), right = std::min(image.right, viewport_.right);
        if (right > left) t->split = std::clamp((p.x - left) / (right - left), 0.03f, 0.97f);
    } else if (drag_ == Drag::move) {
        auto& l = t->doc.active();
        const auto q = imagePoint(p);
        const Point delta{q.x - anchor_.x, q.y - anchor_.y};
        const auto snapped = moveBounds_.empty() ? delta : snappedMove(moveBounds_, delta);
        l.x = std::clamp(moveStart_.x + snapped.x, -maxEdge * 4, maxEdge * 4);
        l.y = std::clamp(moveStart_.y + snapped.y, -maxEdge * 4, maxEdge * 4);
        bitmaps_.clear();
    } else if (drag_ == Drag::lasso) {
        const auto q = imagePoint(p);
        if (lassoPath_.size() < 32768 && q != lassoPath_.back()) lassoPath_.push_back(q);
    } else if (drag_ == Drag::marquee || drag_ == Drag::crop || drag_ == Drag::shape) {
        auto q = snappedPoint(imagePoint(p));
        if (shift() && (drag_ != Drag::marquee || gestureMode_ == SelectMode::replace)) {
            const int size = std::max(std::abs(anchor_.x - q.x), std::abs(anchor_.y - q.y));
            q = {anchor_.x + (q.x < anchor_.x ? -size : size), anchor_.y + (q.y < anchor_.y ? -size : size)};
        }
        gesture_ = drag_ == Drag::marquee ? between(anchor_, q) : intersect(between(anchor_, q), t->doc.state().bounds());
    } else if (drag_ == Drag::colorSV && width(saturation_) > 0) {
        changeHSV(hueValue_, (p.x - saturation_.left) / width(saturation_), 1 - (p.y - saturation_.top) / height(saturation_));
    } else if (drag_ == Drag::colorHue && height(hue_) > 0) {
        changeHSV((p.y - hue_.top) / height(hue_), sat_, val_);
    } else if (drag_ == Drag::layerRow) {
        for (const auto& hit : hits_) if (hit.kind == Hit::layer && p.y >= hit.rect.top && p.y < hit.rect.bottom) dropLayer_ = hit.id;
    }
    last_ = p;
    repaint();
}
void App::leftUp(Vec p) {
    if (drag_ == Drag::none) return;
    if (p.x != last_.x || p.y != last_.y) moveMouse(p);
    auto t = tab();
    const auto drag = std::exchange(drag_, Drag::none);
    ReleaseCapture();
    if (!t) return;
    auto& d = t->doc;
    switch (drag) {
    case Drag::paint: {
        const auto v = screenToImage(p);
        if (std::hypot(lastStamp_.x - v.x, lastStamp_.y - v.y) > 0.5f) {
            Rect dirty;
            dab(v.x, v.y, dirty);
            invalidate(dirty);
        }
        d.commit(tool_ == Tool::eraser ? L"Eraser" : tool_ == Tool::clone ? L"Clone Stamp" : L"Brush Stroke");
        thumbs_.clear(); documentChanged(false); break;
    }
    case Drag::move: d.commit(L"Move Layer"); documentChanged(); break;
    case Drag::guide: finishGuide(p); movingGuide_.reset(); break;
    case Drag::marquee:
        if (std::hypot(p.x - down_.x, p.y - down_.y) < 2.0f) {
            if (gestureMode_ == SelectMode::replace) setSelection({});
        } else if (gesture_) {
            const auto bounds = d.state().bounds();
            const auto region = tool_ == Tool::ellipticalMarquee ? ellipseSelection(*gesture_, bounds) :
                Selection(intersect(*gesture_, bounds));
            setSelection(combineSelection(d.selection.value_or(Selection{}), region, gestureMode_));
        }
        break;
    case Drag::lasso: finishLasso(); break;
    case Drag::crop: t->cropBox = gesture_ && !gesture_->empty() ? gesture_ : std::nullopt; break;
    case Drag::gradient:
        gradient(d.active(), clip(), anchor_, imagePoint(p), foreground_, background_, t->mask, selection());
        d.commit(L"Gradient"); documentChanged(); break;
    case Drag::shape:
        if (gesture_ && !gesture_->empty()) {
            if (tool_ == Tool::ellipse) ellipse(d.active(), *gesture_, clip(), foreground_, selection());
            else fill(d.active(), intersect(*gesture_, clip()), foreground_, false, false, selection());
        }
        d.commit(tool_ == Tool::ellipse ? L"Ellipse" : L"Rectangle"); documentChanged(); break;
    case Drag::layerRow:
        if (draggedLayer_ != dropLayer_ && std::abs(p.y - down_.y) > 5) {
            d.begin();
            auto& state = d.edit();
            auto moved = std::move(state.layers[draggedLayer_]);
            state.layers.erase(state.layers.begin() + draggedLayer_);
            state.layers.insert(state.layers.begin() + dropLayer_, std::move(moved));
            state.active = dropLayer_;
            d.commit(L"Reorder Layers"); documentChanged();
        }
        break;
    default: break;
    }
    gesture_.reset();
    repaint();
}
void App::cancelDrag() {
    const auto drag = std::exchange(drag_, Drag::none);
    if (GetCapture() == window_) ReleaseCapture();
    if (tab()) {
        tab()->doc.cancel();
        if (drag == Drag::transform && tab()->transform) tab()->transform = transformStart_;
    }
    if (drag == Drag::lasso) lassoPath_.clear();
    if (drag == Drag::guide) movingGuide_.reset();
    gesture_.reset();
    documentChanged();
}
bool App::key(WPARAM key) {
    if (key == VK_ESCAPE) {
        if (drag_ != Drag::none) {
            const bool transform = drag_ == Drag::transform;
            cancelDrag();
            if (transform) resetTransform();
            return true;
        }
        if (!lassoPath_.empty()) { lassoPath_.clear(); repaint(); return true; }
        if (tab() && tab()->transform) resetTransform();
        else if (tab() && tab()->cropBox) { tab()->cropBox.reset(); repaint(); }
        else if (tab() && tab()->compare) { command(Command::compare); }
        else if (screenMode_) setScreenMode(0);
        else command(Command::deselect);
        return true;
    }
    if (key == VK_SPACE) { space_ = true; return true; }
    if (drag_ != Drag::none) return false;
    if (key == VK_RETURN && tool_ == Tool::polygonLasso && !lassoPath_.empty()) { finishLasso(); return true; }
    if (key == VK_BACK && tool_ == Tool::polygonLasso && !lassoPath_.empty()) { lassoPath_.pop_back(); repaint(); return true; }
    if (key == VK_RETURN && tab() && tab()->transform) { command(Command::applyTransform); return true; }
    if (key == VK_RETURN && tab() && tab()->cropBox) { command(Command::cropSelection); return true; }
    if (control() && key == VK_TAB && !tabs_.empty()) {
        switchTab((current_ + (shift() ? tabs_.size() - 1 : 1)) % tabs_.size()); return true;
    }
    const bool ctrl = control(), shifted = shift(), alternate = alt();
    Command c = Command::none;
    if (ctrl) {
        switch (key) {
        case 'N': c = shifted ? (alternate ? Command::addLayer : Command::newLayerDialog) : Command::newDoc; break;
        case 'O': c = Command::open; break;
        case 'S': c = shifted ? (alternate ? Command::exportFile : Command::saveAs) : Command::save; break;
        case 'W': c = Command::close; break;
        case 'Z': c = shifted ? Command::redo : Command::undo; break;
        case 'Y': c = Command::redo; break;
        case 'X': c = Command::cut; break;
        case 'C': c = alternate ? Command::canvasSize : shifted ? Command::copyMerged : Command::copy; break;
        case 'V': c = Command::paste; break;
        case 'J': c = shifted ? Command::layerViaCut : Command::duplicate; break;
        case 'A': c = Command::selectAll; break;
        case 'D': c = shifted ? Command::reselect : Command::deselect; break;
        case 'I': c = alternate ? Command::resize : shifted ? Command::inverseSelection : Command::invert; break;
        case 'H': c = Command::extras; break;
        case VK_OEM_1: c = alternate ? Command::lockGuides : shifted ? Command::snap : Command::guides; break;
        case 'K': if (shifted && alternate) c = Command::shortcuts; break;
        case 'U': if (shifted) c = Command::gray; break;
        case 'R': c = Command::rulers; break;
        case 'T': c = Command::freeTransform; break;
        case 'E': c = shifted ? Command::mergeVisible : Command::mergeDown; break;
        case '0': c = Command::fit; break;
        case '1': c = Command::actual; break;
        case VK_BACK: c = Command::fillBG; break;
        case VK_OEM_PLUS: case VK_ADD: c = Command::zoomIn; break;
        case VK_OEM_MINUS: case VK_SUBTRACT: c = Command::zoomOut; break;
        case VK_OEM_4: c = Command::layerDown; break;
        case VK_OEM_6: c = Command::layerUp; break;
        }
    } else if (alternate) {
        if (key == VK_BACK) c = Command::fillFG;
        else if (key == 'F') { menu(0, {42, 28}); return true; }
    } else {
        if (key == VK_TAB) c = shifted ? Command::rightPanels : Command::panels;
        if (key == 'F' && tab()) { setScreenMode((screenMode_ + (shifted ? 2 : 1)) % 3); return true; }
        if (key == VK_DELETE) c = Command::clear;
        if (key == VK_OEM_5 || key == VK_OEM_102) c = Command::compare;
        if (key == 'X') { std::swap(foreground_, background_); updateHSV(); repaint(); return true; }
        if (key == 'D') { foreground_ = rgba(0, 0, 0); background_ = rgba(255, 255, 255); updateHSV(); repaint(); return true; }
        if (key == VK_OEM_4 || key == VK_OEM_6) {
            const int direction = key == VK_OEM_4 ? -1 : 1;
            if (shifted) hardness_ = std::clamp(hardness_ + direction * 25, 0, 100);
            else brushSize_ = std::clamp(brushSize_ + direction * std::max(1, brushSize_ / 8), 1, 2000);
            repaint(); return true;
        }
        if (key >= '0' && key <= '9' && (tool_ == Tool::brush || tool_ == Tool::clone || tool_ == Tool::eraser)) {
            flow_ = key == '0' ? 100 : int(key - '0') * 10;
            repaint(); return true;
        }
        if (key >= VK_LEFT && key <= VK_DOWN && tab() && tab()->transform) {
            auto& area = tab()->transform->box;
            const int delta = shifted ? 10 : 1;
            area.x = std::clamp(area.x + (key == VK_LEFT ? -delta : key == VK_RIGHT ? delta : 0), -maxEdge * 4, maxEdge * 4);
            area.y = std::clamp(area.y + (key == VK_UP ? -delta : key == VK_DOWN ? delta : 0), -maxEdge * 4, maxEdge * 4);
            bitmaps_.clear();
            repaint(); return true;
        }
        if (key >= VK_LEFT && key <= VK_DOWN && tab() && tool_ == Tool::move &&
            !tab()->doc.active().locked && !tab()->doc.active().adjustment) {
            auto& d = tab()->doc;
            d.begin();
            const int delta = shifted ? 10 : 1;
            d.active().x = std::clamp(d.active().x + (key == VK_LEFT ? -delta : key == VK_RIGHT ? delta : 0), -maxEdge * 4, maxEdge * 4);
            d.active().y = std::clamp(d.active().y + (key == VK_UP ? -delta : key == VK_DOWN ? delta : 0), -maxEdge * 4, maxEdge * 4);
            d.commit(L"Nudge Layer"); documentChanged(); return true;
        }
        if (key >= 'A' && key <= 'Z') {
            const auto first = std::find_if(std::begin(tools), std::end(tools), [&](const ToolInfo& item) { return key == WPARAM(item.key); });
            if (first != std::end(tools)) {
                const auto end = std::find_if(first, std::end(tools), [&](const ToolInfo& item) { return item.key != first->key; });
                const auto preferred = preferredTools_[key - 'A'];
                auto chosen = preferred ? std::find_if(first, end, [&](const ToolInfo& item) { return item.tool == *preferred; }) : first;
                if (shifted && chosen != end) ++chosen;
                setTool(chosen == end ? first->tool : chosen->tool);
                return true;
            }
        }
    }
    if (c == Command::none) return false;
    command(c); return true;
}
} // namespace pf
