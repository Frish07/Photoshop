#include "app.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace pf {
namespace {
constexpr unsigned ink = 0xe6e7e9, muted = 0xabaeb5, faint = 0x81858f, border = 0x1b1c1e;
constexpr unsigned menuBg = 0x292a2d, panelBg = 0x303134, sub = 0x35363a, fieldBg = 0x27282b;
constexpr unsigned selected = 0x424953, blueAccent = 0x86bff4;
constexpr float toolbarWidth = 52, ruler = 20, tabsTop = 70, tabsBottom = 104, bottomBar = 28;
constexpr const wchar_t* labels[]{L"File", L"Edit", L"Image", L"Layer", L"Select", L"Filter", L"View", L"Window", L"Help"};
constexpr float labelWidths[]{37, 39, 54, 49, 51, 46, 45, 66, 46};
constexpr const wchar_t* modes[]{L"Normal", L"Multiply", L"Screen", L"Overlay", L"Darken", L"Lighten", L"Difference"};
constexpr DWRITE_TEXT_ALIGNMENT center = DWRITE_TEXT_ALIGNMENT_CENTER;
constexpr DWRITE_TEXT_ALIGNMENT right = DWRITE_TEXT_ALIGNMENT_TRAILING;

D2D1_COLOR_F color(unsigned rgb, float alpha = 1) { return D2D1::ColorF(rgb & 0xffffffu, alpha); }
Pixel hsv(float h, float s, float v) {
    auto channel = [&](float offset) {
        const float k = std::fmod(offset + h * 6, 6.0f);
        return int(255 * (v - v * s * std::max(0.0f, std::min({k, 4 - k, 1.0f}))) + 0.5f);
    };
    return rgba(channel(5), channel(3), channel(1));
}
std::wstring percentage(float value) {
    std::wostringstream out;
    if (value < 1 && std::abs(value * 100 - std::round(value * 100)) > 0.04f)
        out << std::fixed << std::setprecision(1) << value * 100;
    else out << int(value * 100 + 0.5f);
    return out.str() + L"%";
}
std::wstring hex(Pixel c) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring s = L"#000000";
    for (int i = 0; i < 6; ++i) s[6 - i] = digits[(c >> (i * 4)) & 15];
    return s;
}
std::wstring memory(std::size_t n) {
    if (n < (1u << 20)) return std::to_wstring((n + 512) >> 10) + L" KB";
    std::wostringstream out;
    out << std::fixed << std::setprecision(n < (10u << 20) ? 1 : 0) << n / 1048576.0 << L" MB";
    return out.str();
}
Box overlap(Box a, Box b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
}
} // namespace

bool App::createTarget() {
    if (target_) return true;
    if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.put()))) return false;
    if (!write_) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write_.put())))) return false;
        constexpr float sizes[]{12, 12, 11, 19, 34, 10, 14};
        for (int i = 0; i < int(fonts_.size()); ++i) {
            if (FAILED(write_->CreateTextFormat(L"Segoe UI", nullptr, i == 1 || i == 3 || i == 4 ? DWRITE_FONT_WEIGHT_SEMI_BOLD :
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                sizes[i], L"", fonts_[i].put()))) return false;
            fonts_[i]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fonts_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    RECT r{};
    GetClientRect(window_, &r);
    if (r.right <= 0 || r.bottom <= 0) return false;
    if (FAILED(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(), dpi_ * 96, dpi_ * 96), D2D1::HwndRenderTargetProperties(window_,
        D2D1::SizeU(r.right, r.bottom)), target_.put()))) return false;
    return SUCCEEDED(target_->CreateSolidColorBrush(color(ink), brush_.put()));
}
void App::layout() {
    const auto size = target_->GetSize();
    w_ = size.width; h_ = size.height;
    const float sidebarWidth = showPanels_ ? std::clamp(w_ * 0.208f, 244.0f, 306.0f) : 0;
    const float toolsWidth = showPanels_ ? toolbarWidth : 0;
    sidebar_ = {w_ - sidebarWidth, tabsTop, w_, h_ - bottomBar};
    viewport_ = {toolsWidth + (showRulers_ ? ruler : 0), tabsBottom + (showRulers_ ? ruler : 0),
        sidebar_.left, h_ - bottomBar};
    if (auto t = tab(); t && t->fit) {
        t->zoom = std::clamp(std::min((width(viewport_) - 90) / t->doc.state().width,
            (height(viewport_) - 94) / t->doc.state().height), 0.0125f, 1.0f);
        t->pan = {};
    }
}
void App::paint() {
    if (!createTarget()) return;
    layout();
    ++frame_;
    hits_.clear();
    target_->BeginDraw();
    target_->Clear(color(0x202123));
    try {
        drawHeader();
        if (tab()) {
            drawCanvas();
            drawRulers();
        } else drawHome();
        if (showPanels_) { drawToolbar(); drawSidebar(); }
        drawStatus();
        drawTooltip();
    } catch (...) { target_->EndDraw(); throw; }
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
        clearCache(); checkerBrush_.reset(); checker_.reset(); spectrum_.reset(); hueStrip_.reset();
        brush_.reset(); target_.reset();
        repaint();
    }
}
void App::rect(Box r, Color c, float a) {
    if (width(r) <= 0 || height(r) <= 0) return;
    brush_->SetColor(color(c, a)); target_->FillRectangle(r, brush_.get());
}
void App::strokeRect(Box r, Color c, float weight) {
    brush_->SetColor(color(c)); target_->DrawRectangle(r, brush_.get(), weight);
}
void App::rounded(Box r, float radius, Color c, float a) {
    brush_->SetColor(color(c, a)); target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.get());
}
void App::line(float x1, float y1, float x2, float y2, Color c, float weight) {
    brush_->SetColor(color(c)); target_->DrawLine({x1, y1}, {x2, y2}, brush_.get(), weight);
}
void App::circle(float x, float y, float r, Color c, bool fill, float weight) {
    brush_->SetColor(color(c));
    const auto el = D2D1::Ellipse({x, y}, r, r);
    if (fill) target_->FillEllipse(el, brush_.get());
    else target_->DrawEllipse(el, brush_.get(), weight);
}
void App::text(Box r, std::wstring_view s, Color c, int style, DWRITE_TEXT_ALIGNMENT align) {
    if (s.empty() || width(r) <= 0 || height(r) <= 0) return;
    fonts_[style]->SetTextAlignment(align);
    brush_->SetColor(color(c));
    target_->DrawText(s.data(), UINT32(s.size()), fonts_[style].get(), r, brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void App::hit(Box r, Hit kind, int id, std::wstring tip) { hits_.push_back({r, kind, id, std::move(tip)}); }
void App::button(Box r, std::wstring_view s, Hit kind, int id, bool on, std::wstring tip) {
    const bool hover = inside(r, cursor_);
    rounded(r, 4, on ? selected : hover ? 0x47494e : fieldBg);
    text(r, s, on ? blueAccent : ink, 0, center);
    hit(r, kind, id, std::move(tip));
}
void App::iconButton(Box r, Icon i, Hit kind, int id, bool on, std::wstring tip) {
    const bool hover = inside(r, cursor_);
    if (on || hover) rounded(r, 4, on ? selected : 0x47494e);
    icon(i, (r.left + r.right) / 2, (r.top + r.bottom) / 2, on ? blueAccent : ink);
    hit(r, kind, id, std::move(tip));
}
void App::icon(Icon i, float x, float y, Color c) {
    auto l = [&](float a, float b, float d, float e) { line(x + a, y + b, x + d, y + e, c, 1.35f); };
    auto r = [&](float a, float b, float w, float h) { strokeRect(box(x + a, y + b, w, h), c, 1.25f); };
    auto o = [&](float a, float b, float radius, bool fill = false) { circle(x + a, y + b, radius, c, fill, 1.25f); };
    switch (i) {
    case Icon::move:
        l(-8, 0, 8, 0); l(0, -8, 0, 8);
        l(-8, 0, -5, -3); l(-8, 0, -5, 3); l(8, 0, 5, -3); l(8, 0, 5, 3);
        l(0, -8, -3, -5); l(0, -8, 3, -5); l(0, 8, -3, 5); l(0, 8, 3, 5); break;
    case Icon::marquee:
        for (int a = -7; a < 7; a += 5) { l(a, -6, a + 3, -6); l(a, 6, a + 3, 6); }
        for (int a = -5; a < 6; a += 4) { l(-7, a, -7, a + 2); l(7, a, 7, a + 2); } break;
    case Icon::crop:
        l(-5, -9, -5, 5); l(-5, 5, 9, 5); l(5, 9, 5, -5); l(5, -5, -9, -5); l(-3, 3, 8, -8); break;
    case Icon::picker:
        l(-7, 7, -7, 3); l(-7, 3, 3, -7); l(3, -7, 7, -3); l(7, -3, -3, 7); l(-3, 7, -7, 7); l(0, -4, 4, 0); break;
    case Icon::brush:
        l(-2, 3, 4, -7); l(4, -7, 7, -8); l(7, -8, 8, -5); l(8, -5, 1, 3);
        l(1, 3, 0, 6); l(0, 6, -4, 8); l(-4, 8, -8, 8); l(-8, 8, -6, 5); l(-6, 5, -5, 3); l(-5, 3, -2, 3); break;
    case Icon::eraser:
        l(-7, 2, 2, -7); l(2, -7, 8, -1); l(8, -1, 0, 7); l(0, 7, -3, 7); l(-3, 7, -7, 2);
        l(-3, -2, 3, 4); l(0, 7, 9, 7); break;
    case Icon::bucket:
        l(-8, 0, -2, -6); l(-2, -6, 5, 1); l(5, 1, -1, 7); l(-1, 7, -8, 0); l(-7, 0, 4, 0);
        l(-5, -8, -1, -4); o(8, 5, 1.8f, true); break;
    case Icon::gradient:
        r(-8, -5, 16, 10); rect(box(x - 7, y - 4, 4, 8), c, 0.82f);
        rect(box(x - 3, y - 4, 4, 8), c, 0.55f); rect(box(x + 1, y - 4, 4, 8), c, 0.29f); break;
    case Icon::type:
        l(-7, -7, 7, -7); l(-7, -7, -7, -4); l(7, -7, 7, -4); l(0, -7, 0, 7); l(-4, 7, 4, 7); break;
    case Icon::rectangle: r(-7, -5, 14, 10); break;
    case Icon::ellipse:
        brush_->SetColor(color(c)); target_->DrawEllipse(D2D1::Ellipse({x, y}, 7.5f, 5.5f), brush_.get(), 1.3f); break;
    case Icon::hand:
        l(-5, 0, -5, -5); l(-5, -5, -3, -6); l(-3, -6, -2, -1); l(-2, -1, -2, -8);
        l(-2, -8, 0, -8); l(0, -8, 1, -1); l(1, -1, 1, -7); l(1, -7, 3, -7); l(3, -7, 4, 0);
        l(4, 0, 5, -4); l(5, -4, 7, -4); l(7, -4, 7, 3); l(7, 3, 4, 8); l(4, 8, -2, 8);
        l(-2, 8, -8, 1); l(-8, 1, -7, -1); l(-7, -1, -5, 0); break;
    case Icon::zoom: o(-2, -2, 5); l(2, 2, 7, 7); l(-4, -2, 0, -2); l(-2, -4, -2, 0); break;
    case Icon::more: o(-5, 0, 1.2f, true); o(0, 0, 1.2f, true); o(5, 0, 1.2f, true); break;
    case Icon::plus: l(-5, 0, 5, 0); l(0, -5, 0, 5); break;
    case Icon::close: l(-3, -3, 3, 3); l(-3, 3, 3, -3); break;
    case Icon::eye: case Icon::hidden:
        if (i == Icon::hidden) { l(-4, 0, 4, 0); break; }
        l(-8, 0, -5, -3); l(-5, -3, 0, -4); l(0, -4, 5, -3); l(5, -3, 8, 0);
        l(-8, 0, -5, 3); l(-5, 3, 0, 4); l(0, 4, 5, 3); l(5, 3, 8, 0); o(0, 0, 1.75f, true); break;
    case Icon::lock:
        r(-4, -1, 8, 7); l(-2, -1, -2, -5); l(-2, -5, 2, -5); l(2, -5, 2, -1); o(0, 2.5f, 0.8f, true); break;
    case Icon::mask: r(-7, -5, 14, 10); o(0, 0, 2.8f); break;
    case Icon::trash:
        l(-6, -4, 6, -4); l(-2, -6, 2, -6); l(-4, -2, -3, 6); l(4, -2, 3, 6); l(-3, 6, 3, 6); l(0, -2, 0, 4); break;
    case Icon::down: l(-4, -2, 0, 2); l(0, 2, 4, -2); break;
    case Icon::up: l(-4, 2, 0, -2); l(0, -2, 4, 2); break;
    case Icon::check: l(-5, 0, -1, 4); l(-1, 4, 6, -4); break;
    case Icon::fit:
        l(-7, -2, -7, -6); l(-7, -6, -3, -6); l(7, -2, 7, -6); l(7, -6, 3, -6);
        l(-7, 2, -7, 6); l(-7, 6, -3, 6); l(7, 2, 7, 6); l(7, 6, 3, 6); r(-3, -2, 6, 4); break;
    case Icon::image:
        r(-8, -6, 16, 12); l(-7, 5, -2, 0); l(-2, 0, 1, 3); l(1, 3, 4, -1); l(4, -1, 7, 3); o(3, -3, 1, true); break;
    case Icon::swap:
        l(-5, -2, 5, -2); l(5, -2, 2, -5); l(-5, 2, 5, 2); l(-5, 2, -2, 5); break;
    case Icon::layers:
        l(-7, -2, 0, -6); l(0, -6, 7, -2); l(7, -2, 0, 2); l(0, 2, -7, -2);
        l(-7, 2, 0, 6); l(0, 6, 7, 2); break;
    }
}

void App::drawHeader() {
    rect(box(0, 0, w_, 30), menuBg);
    rounded(box(10, 4, 24, 22), 5, 0x16304a);
    text(box(10, 4, 24, 22), L"Px", blueAccent, 1, center);
    float x = 42;
    for (int i = 0; i < int(std::size(labels)); ++i) {
        auto area = box(x, 2, labelWidths[i], 26);
        if (inside(area, cursor_)) rounded(area, 4, 0x404247);
        text(area, labels[i], ink, 0, center);
        hit(area, Hit::menu, i);
        x += labelWidths[i];
    }
    if (w_ > 900) {
        text(box(w_ - 208, 0, 194, 30), L"PixelForge  /  Essentials", faint, 0, right);
    }
    rect(box(0, 30, w_, 40), sub);
    line(0, 30.5f, w_, 30.5f, border);
    line(0, 69.5f, w_, 69.5f, border);
    const auto tool = std::find_if(std::begin(tools), std::end(tools), [&](const ToolInfo& t) { return t.tool == tool_; });
    icon(tool->icon, 24, 50, ink);
    text(box(43, 30, 122, 40), tool->name, ink, 1);
    line(166, 40, 166, 60, 0x53545a);
    x = 180;
    auto option = [&](std::wstring_view label, std::wstring value, Hit kind, int w = 62) {
        const auto labelWidth = float(label.size() * 6 + 14);
        text(box(x, 30, labelWidth, 40), label, muted);
        x += labelWidth;
        button(box(x, 37, float(w), 26), value, kind, 0, false, std::wstring(label));
        x += w + 17;
    };
    if (tool_ == Tool::brush || tool_ == Tool::eraser) {
        option(L"Size", std::to_wstring(brushSize_) + L" px", Hit::fgSize);
        option(L"Hardness", std::to_wstring(hardness_) + L"%", Hit::hardness);
        option(L"Opacity", std::to_wstring(flow_) + L"%", Hit::flow);
        if (w_ > 930) {
            circle(x + 3, 50, 7, tool_ == Tool::brush ? foreground_ : muted, tool_ == Tool::brush);
            text(box(x + 18, 30, 120, 40), tool_ == Tool::brush ? L"Soft Round" : L"Round Eraser", faint);
        }
    } else if (tool_ == Tool::bucket) {
        option(L"Tolerance", std::to_wstring(tolerance_), Hit::tolerance);
        text(box(x, 30, 145, 40), L"Contiguous", muted);
    } else if (tool_ == Tool::type) {
        option(L"Size", std::to_wstring(typeSize_) + L" px", Hit::typeSize);
        text(box(x, 30, 180, 40), L"Segoe UI · Raster text", muted);
    } else if (tool_ == Tool::crop) {
        if (tab() && tab()->cropBox) {
            iconButton(box(x, 37, 31, 26), Icon::check, Hit::applyCrop, 0, true, L"Commit crop (Enter)");
            iconButton(box(x + 36, 37, 31, 26), Icon::close, Hit::cancelCrop, 0, false, L"Cancel crop (Esc)");
        } else text(box(x, 30, 280, 40), L"Drag to define the crop area", muted);
    } else if (tool_ == Tool::zoom) {
        button(box(x, 37, 32, 26), L"+", Hit::zoomMode, 0, !zoomOutMode_);
        button(box(x + 36, 37, 32, 26), L"−", Hit::zoomMode, 1, zoomOutMode_);
        button(box(x + 76, 37, 66, 26), L"100%", Hit::command, int(Command::actual));
    } else {
        const wchar_t* hint = tool_ == Tool::marquee ? L"Drag to select pixels · Shift for square" :
            tool_ == Tool::move ? L"Drag to move the selected layer" :
            tool_ == Tool::picker ? L"Sample all visible layers" :
            tool_ == Tool::gradient ? L"Linear · Foreground to Background" :
            tool_ == Tool::hand ? L"Drag to pan" : L"Drag to draw · Shift to constrain";
        text(box(x, 30, 290, 40), hint, muted);
    }
    if (tab()) {
        iconButton(box(w_ - 115, 36, 32, 27), Icon::fit, Hit::fit, 0, false, L"Fit on Screen (Ctrl+0)");
        button(box(w_ - 78, 37, 65, 26), L"Export", Hit::command, int(Command::exportFile));
    }
    const float start = showPanels_ ? toolbarWidth : 0;
    rect(box(start, tabsTop, sidebar_.left - start, tabsBottom - tabsTop), 0x252629);
    line(start, tabsBottom - 0.5f, sidebar_.left, tabsBottom - 0.5f, border);
    float tabX = start;
    for (int index = tabFirst_; index < int(tabs_.size()) && tabX + 72 < sidebar_.left; ++index) {
        const float tabWidth = std::min(204.0f, sidebar_.left - tabX);
        const auto& t = *tabs_[index];
        const auto area = box(tabX, tabsTop, tabWidth, tabsBottom - tabsTop);
        const bool active = index == int(current_);
        rect(area, active ? 0x393b40 : inside(area, cursor_) ? 0x303237 : menuBg);
        line(area.right - 0.5f, tabsTop, area.right - 0.5f, tabsBottom, border);
        if (active) rect(box(tabX, tabsTop, tabWidth, 2), blueAccent);
        icon(Icon::image, tabX + 16, 87, active ? blueAccent : faint);
        auto caption = t.title + (t.doc.dirty() ? L" *" : L"");
        if (caption.size() > 24) caption = caption.substr(0, 22) + L"…";
        text(box(tabX + 30, tabsTop + 1, tabWidth - 57, 32), caption, active ? ink : muted, active ? 1 : 0);
        hit(area, Hit::tab, index, t.title);
        iconButton(box(area.right - 26, 77, 20, 20), Icon::close, Hit::closeTab, index, false, L"Close document");
        tabX += tabWidth;
    }
    if (tabX + 32 < sidebar_.left) iconButton(box(tabX + 3, 75, 29, 25), Icon::plus, Hit::command,
        int(Command::newDoc), false, L"New Document (Ctrl+N)");
}
void App::drawToolbar() {
    const auto area = box(0, tabsTop, toolbarWidth, h_ - bottomBar - tabsTop);
    rect(area, menuBg);
    line(toolbarWidth - 0.5f, tabsTop, toolbarWidth - 0.5f, area.bottom, border);
    float y = tabsTop + 12;
    for (std::size_t i = 0; i < std::size(tools); ++i) {
        const auto& t = tools[i];
        if (y + 77 > area.bottom) break;
        if (i == 3 || i == 8 || i == 11) { line(12, y + 2, 39, y + 2, 0x45474c); y += 9; }
        const auto r = box(8, y, 35, 30);
        const auto label = std::wstring(t.name) + L" (" + t.key + L")";
        iconButton(r, t.icon, Hit::tool, int(t.tool), t.tool == tool_, label);
        if (t.tool == tool_) rect(box(8, y + 5, 2, 20), blueAccent);
        y += 31;
    }
    iconButton(box(8, y + 2, 35, 25), Icon::more, Hit::moreTools, 0, false, L"All Tools");
    y = std::min(y + 44, area.bottom - 43);
    strokeRect(box(23, y + 8, 18, 18), border);
    rect(box(24, y + 9, 16, 16), background_);
    hit(box(29, y + 17, 13, 12), Hit::color, 1, L"Background color");
    strokeRect(box(11, y - 2, 22, 22), 0x9a9ba0);
    rect(box(12, y - 1, 20, 20), foreground_);
    hit(box(11, y - 2, 22, 22), Hit::color, 0, L"Foreground color");
    icon(Icon::swap, 39, y - 4, muted);
    hit(box(33, y - 13, 14, 15), Hit::swap, 0, L"Swap Colors (X)");
    rect(box(11, y + 24, 8, 8), 0xeeeeee);
    rect(box(10, y + 22, 7, 7), 0x070809);
    hit(box(7, y + 21, 14, 15), Hit::defaults, 0, L"Default Colors (D)");
}
void App::drawSidebar() {
    rect(sidebar_, panelBg);
    line(sidebar_.left + 0.5f, sidebar_.top, sidebar_.left + 0.5f, sidebar_.bottom, border);
    const float upper = std::min(252.0f, height(sidebar_) * 0.51f);
    drawColorPanel(box(sidebar_.left + 1, sidebar_.top, width(sidebar_) - 1, upper));
    const float top = sidebar_.top + upper;
    rect(box(sidebar_.left + 1, top, width(sidebar_) - 1, 32), menuBg);
    line(sidebar_.left, top, sidebar_.right, top, border);
    const wchar_t* names[]{L"Layers", L"History"};
    float x = sidebar_.left + 1;
    for (int i = 0; i < 2; ++i) {
        const auto tab = box(x, top + 1, 67, 31);
        if (i == sidebarTab_) {
            rect(tab, panelBg); rect(box(x + 13, top + 30, 41, 2), blueAccent);
        } else if (inside(tab, cursor_)) rect(tab, 0x3b3d41);
        text(tab, names[i], i == sidebarTab_ ? ink : muted, i == sidebarTab_ ? 1 : 0, center);
        hit(tab, Hit::panel, i);
        x += 67;
    }
    if (!tab()) {
        text(box(sidebar_.left + 12, top + 52, width(sidebar_) - 24, 22), L"No document open", faint, 0, center);
        return;
    }
    auto lower = Box{sidebar_.left + 1, top + 32, sidebar_.right, sidebar_.bottom};
    if (sidebarTab_ == 0) drawLayers(lower);
    else drawHistory(lower);
}
void App::drawColorPanel(Box area) {
    rect(box(area.left, area.top, width(area), 34), menuBg);
    const wchar_t* names[]{L"Color", L"Swatches"};
    for (int i = 0; i < 2; ++i) {
        const auto r = box(area.left + i * 69, area.top, 69, 34);
        if (i == colorTab_) { rect(r, panelBg); rect(box(r.left + 13, r.bottom - 2, 43, 2), blueAccent); }
        else if (inside(r, cursor_)) rect(r, 0x3b3d41);
        text(r, names[i], i == colorTab_ ? ink : muted, i == colorTab_ ? 1 : 0, center);
        hit(r, Hit::colorTab, i);
    }
    if (colorTab_ == 1) {
        constexpr unsigned colors[]{
            0x131416, 0x34353a, 0x686a73, 0x90949f, 0xc6c7cb, 0xffffff, 0xfff1d8, 0xe3c48d,
            0xc97e4d, 0xaa4938, 0xdf625c, 0xec9674, 0xfac176, 0xe6cb56, 0x9eaa67, 0x49785f,
            0x1b554f, 0x358c84, 0x4cabb4, 0x7dbdd8, 0x4274b7, 0x40508b, 0x886da9, 0xc68cac,
            0x293e4e, 0x344a60, 0x57697c, 0x80939e, 0xadbbbf, 0x5d594e, 0x867463, 0xa89a83};
        const float edge = std::min(30.0f, (width(area) - 35) / 8);
        for (int i = 0; i < int(std::size(colors)); ++i) {
            auto r = box(area.left + 14 + i % 8 * edge, area.top + 47 + i / 8 * edge, edge - 4, edge - 4);
            rect(r, colors[i]);
            if (inside(r, cursor_) || (foreground_ & 0xffffffu) == colors[i]) strokeRect(r, ink, 1.5f);
            hit(r, Hit::swatch, int(colors[i]), hex(colors[i]));
        }
        text(box(area.left + 14, area.bottom - 34, width(area) - 28, 24), hex(foreground_), muted, 1);
        return;
    }
    const float x = area.left + 15, y = area.top + 47;
    saturation_ = box(x, y, width(area) - 56, height(area) - 103);
    hue_ = box(saturation_.right + 9, y, 15, height(saturation_));
    const auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (!spectrum_) {
        constexpr int n = 72;
        std::array<Pixel, n * n> pixels;
        for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i)
            pixels[j * n + i] = hsv(hueValue_, i / float(n - 1), 1 - j / float(n - 1));
        target_->CreateBitmap(D2D1::SizeU(n, n), pixels.data(), n * 4, properties, spectrum_.put());
    }
    if (!hueStrip_) {
        std::array<Pixel, 180> pixels;
        for (int i = 0; i < int(pixels.size()); ++i) pixels[i] = hsv(i / 179.0f, 0.94f, 0.96f);
        target_->CreateBitmap(D2D1::SizeU(1, 180), pixels.data(), 4, properties, hueStrip_.put());
    }
    if (spectrum_) target_->DrawBitmap(spectrum_.get(), saturation_);
    if (hueStrip_) target_->DrawBitmap(hueStrip_.get(), hue_);
    strokeRect(saturation_, border); strokeRect(hue_, border);
    const float markerX = saturation_.left + sat_ * width(saturation_);
    const float markerY = saturation_.top + (1 - val_) * height(saturation_);
    circle(markerX, markerY, 5.5f, 0x191a1c, false, 2);
    circle(markerX, markerY, 4.8f, 0xf7f7f7, false, 1.25f);
    strokeRect(box(hue_.left - 2, hue_.top + hueValue_ * height(hue_) - 2, width(hue_) + 4, 4), ink);
    hit(saturation_, Hit::saturation); hit(hue_, Hit::hue);
    const auto swatch = box(x, area.bottom - 38, 23, 23);
    rounded(swatch, 3, foreground_); hit(swatch, Hit::color, 0, L"Foreground color");
    text(box(x + 31, area.bottom - 39, 67, 25), hex(foreground_), ink, 1);
    text(box(x + 105, area.bottom - 39, width(area) - 137, 25),
        L"R " + std::to_wstring(red(foreground_)) + L"  G " + std::to_wstring(green(foreground_)) +
        L"  B " + std::to_wstring(blue(foreground_)), muted, 2, right);
}
void App::chequer(Box area, float size, Color c1, Color c2) {
    if (width(area) <= 0 || height(area) <= 0) return;
    rect(area, c1);
    for (int y = 0; y * size < height(area); ++y) {
        for (int x = 1 - (y & 1); x * size < width(area); x += 2) {
            const auto r = overlap(box(area.left + x * size, area.top + y * size, size, size), area);
            rect(r, c2);
        }
    }
}
void App::thumb(Box area, const Layer& layer, bool mask) {
    chequer(area, 5, 0x64666a, 0x7a7c80);
    if (mask) {
        const auto& plane = *layer.mask;
        const auto key = plane.identity();
        auto found = thumbs_.find(key);
        if (found == thumbs_.end() && drag_ != Drag::paint) {
            const auto bounds = layer.pixels.extent().empty() ? Rect{0, 0, tileEdge, tileEdge} : layer.pixels.extent();
            std::array<Pixel, 32 * 32> pixels;
            for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x)
                pixels[y * 32 + x] = plane.pixel(bounds.x + x * bounds.w / 32, bounds.y + y * bounds.h / 32);
            Thumbnail t{layer.pixels, layer.mask, {}};
            target_->CreateBitmap(D2D1::SizeU(32, 32), pixels.data(), 32 * 4,
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), t.image.put());
            found = thumbs_.emplace(key, std::move(t)).first;
        }
        if (found != thumbs_.end() && found->second.image) target_->DrawBitmap(found->second.image.get(), area);
        return;
    }
    if (layer.pixels.tiles().empty()) return;
    const auto key = layer.pixels.identity();
    auto found = thumbs_.find(key);
    if (found == thumbs_.end() && drag_ != Drag::paint) {
        const auto bounds = layer.pixels.extent();
        const float ratio = std::min(36.0f / bounds.w, 30.0f / bounds.h);
        const int w = std::max(1, int(bounds.w * ratio + 0.5f)), h = std::max(1, int(bounds.h * ratio + 0.5f));
        std::array<Pixel, 36 * 30> pixels{};
        const int left = (36 - w) / 2, top = (30 - h) / 2;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            pixels[(top + y) * 36 + left + x] = premultiply(layer.pixels.pixel(bounds.x + x * bounds.w / w, bounds.y + y * bounds.h / h));
        Thumbnail t{layer.pixels, std::nullopt, {}};
        target_->CreateBitmap(D2D1::SizeU(36, 30), pixels.data(), 36 * 4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), t.image.put());
        found = thumbs_.emplace(key, std::move(t)).first;
    }
    if (found != thumbs_.end() && found->second.image) target_->DrawBitmap(found->second.image.get(), area);
}
void App::drawLayers(Box area) {
    const auto t = tab();
    const auto& state = t->doc.state();
    const auto& layer = t->doc.active();
    const float x = area.left + 12, y = area.top + 10;
    const auto blend = box(x, y, width(area) * 0.48f, 26);
    rounded(blend, 4, inside(blend, cursor_) ? 0x45474d : fieldBg);
    text(box(x + 9, y, width(blend) - 27, 26), modes[std::size_t(layer.blend)], ink);
    icon(Icon::down, blend.right - 12, y + 13, muted);
    hit(blend, Hit::blend, 0, L"Layer blending mode");
    const auto op = box(blend.right + 10, y, area.right - 12 - blend.right - 10, 26);
    rounded(op, 4, inside(op, cursor_) ? 0x45474d : fieldBg);
    text(box(op.left + 8, y, 52, 26), L"Opacity", muted, 2);
    text(box(op.right - 44, y, 36, 26), std::to_wstring((layer.opacity * 100 + 127) / 255) + L"%", ink, 0, right);
    hit(op, Hit::opacity, 0, L"Change layer opacity");
    const auto rowTop = area.top + 45;
    const auto footer = area.bottom - 35;
    const int visible = std::max(0, int((footer - rowTop) / 45));
    t->layerScroll = std::clamp(t->layerScroll, 0, std::max(0, int(state.layers.size()) - visible));
    line(area.left, rowTop - 0.5f, area.right, rowTop - 0.5f, border);
    for (int i = 0; i < visible; ++i) {
        const int index = int(state.layers.size()) - 1 - t->layerScroll - i;
        if (index < 0) break;
        const auto& l = state.layers[index];
        const bool active = index == state.active;
        const auto r = box(area.left, rowTop + i * 45, width(area), 45);
        rect(r, active ? selected : inside(r, cursor_) ? 0x383a3e : 0x323337);
        if (active) rect(box(r.left, r.top, 2, 45), blueAccent);
        line(r.left, r.bottom - 0.5f, r.right, r.bottom - 0.5f, 0x26272a);
        if (drag_ == Drag::layerRow && dropLayer_ == index && index != draggedLayer_)
            line(r.left + 5, r.top + 1, r.right - 5, r.top + 1, blueAccent, 2);
        hit(r, Hit::layer, index, L"Drag to reorder layers");
        iconButton(box(r.left + 4, r.top + 9, 23, 27), l.visible ? Icon::eye : Icon::hidden, Hit::eye, index, false, L"Toggle layer visibility");
        const auto preview = box(r.left + 34, r.top + 8, 36, 29);
        thumb(preview, l, false);
        strokeRect(preview, active && !t->mask ? blueAccent : 0x202124);
        float tx = preview.right + 10;
        if (l.mask) {
            const auto mask = box(preview.right + 5, preview.top, 27, 29);
            thumb(mask, l, true);
            strokeRect(mask, active && t->mask ? blueAccent : 0x202124);
            hit(mask, Hit::mask, index, L"Select layer mask");
            tx = mask.right + 10;
        }
        text(box(tx, r.top, r.right - tx - 37, 45), l.name, active ? ink : muted, active ? 1 : 0);
        if (l.locked || active || inside(r, cursor_)) {
            if (l.locked) iconButton(box(r.right - 31, r.top + 10, 24, 25), Icon::lock, Hit::lock, index, false, L"Unlock layer");
            else if (inside(box(r.right - 31, r.top + 10, 24, 25), cursor_))
                iconButton(box(r.right - 31, r.top + 10, 24, 25), Icon::lock, Hit::lock, index, false, L"Lock layer");
            else hit(box(r.right - 31, r.top + 10, 24, 25), Hit::lock, index, L"Lock layer");
        }
    }
    if (visible < int(state.layers.size()) && visible > 0) {
        const float bar = std::max(12.0f, (footer - rowTop) * visible / float(state.layers.size()));
        const float offset = (footer - rowTop - bar) * t->layerScroll / std::max(1, int(state.layers.size()) - visible);
        rounded(box(area.right - 3, rowTop + offset, 2, bar), 1, 0x686b73);
    }
    rect(box(area.left, footer, width(area), 35), menuBg);
    line(area.left, footer + 0.5f, area.right, footer + 0.5f, border);
    text(box(area.left + 10, footer, 90, 35), std::to_wstring(state.layers.size()) +
        (state.layers.size() == 1 ? L" layer" : L" layers"), faint, 2);
    const std::array icons{Icon::down, Icon::up, Icon::mask, Icon::plus, Icon::trash};
    const std::array actions{Command::layerDown, Command::layerUp, Command::addMask, Command::addLayer, Command::deleteLayer};
    const wchar_t* tips[]{L"Send Backward", L"Bring Forward", L"Add Layer Mask", L"New Layer (Ctrl+Shift+N)", L"Delete Layer"};
    for (int i = 0; i < 5; ++i) iconButton(box(area.right - 141 + i * 26, footer + 5, 25, 25), icons[i], Hit::command,
        int(actions[i]), false, tips[i]);
}
void App::drawHistory(Box area) {
    const auto t = tab();
    const auto& d = t->doc;
    const int count = int(d.historyCount());
    const int visible = std::max(0, int((height(area) - 43) / 29));
    t->historyScroll = std::clamp(t->historyScroll, 0, std::max(0, count - visible));
    const int start = std::max(0, count - visible - t->historyScroll);
    for (int i = 0; i < visible && i + start < count; ++i) {
        const int pos = i + start;
        const auto r = box(area.left, area.top + 6 + i * 29, width(area), 29);
        const bool active = pos == int(d.historyIndex());
        if (active || inside(r, cursor_)) rect(r, active ? selected : sub);
        circle(r.left + 17, r.top + 14, 2.5f, active ? blueAccent : faint, active);
        text(box(r.left + 28, r.top, width(r) - 37, 29), d.historyLabel(pos), active ? ink : pos > int(d.historyIndex()) ? faint : muted);
        hit(r, Hit::history, pos, L"Restore this history state");
    }
    line(area.left, area.bottom - 35, area.right, area.bottom - 35, border);
    text(box(area.left + 12, area.bottom - 34, width(area) - 24, 34), memory(d.historyBytes()) + L" / 96 MB history", muted, 2);
}
void App::cacheTile(Rect area, int step, Coord key) {
    const int w = (area.w + step - 1) / step, h = (area.h + step - 1) / step;
    std::array<Pixel, tileEdge * tileEdge> buffer;
    auto pixels = std::span(buffer).first(std::size_t(w) * h);
    preview(tab()->doc.state(), area, step, w, h, pixels);
    Bitmap entry{area, {}, frame_};
    if (std::any_of(pixels.begin(), pixels.end(), [](Pixel p) { return p != 0; })) {
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(target_->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, props, entry.image.put()))) return;
    }
    bitmaps_.emplace(key, std::move(entry));
}
void App::drawCanvas() {
    auto t = tab();
    const auto& state = t->doc.state();
    target_->PushAxisAlignedClip(viewport_, D2D1_ANTIALIAS_MODE_ALIASED);
    rect(viewport_, 0x202123);
    const auto image = imageRect();
    for (int i = 9; i > 0; --i) rect(box(image.left - i / 2.0f, image.top + 2 - i / 4.0f,
        width(image) + i, height(image) + i), 0x070809, 0.035f);
    const auto visible = overlap(viewport_, image);
    if (width(visible) > 0 && height(visible) > 0) {
        target_->PushAxisAlignedClip(visible, D2D1_ANTIALIAS_MODE_ALIASED);
        if (!checkerBrush_) {
            std::array<Pixel, 20 * 20> pattern;
            for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x)
                pattern[y * 20 + x] = (x < 10) == (y < 10) ? 0xffb8b8b8 : 0xff939396;
            const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (SUCCEEDED(target_->CreateBitmap(D2D1::SizeU(20, 20), pattern.data(), 80, props, checker_.put())))
                target_->CreateBitmapBrush(checker_.get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,
                    D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), checkerBrush_.put());
        }
        if (checkerBrush_) {
            checkerBrush_->SetTransform(D2D1::Matrix3x2F::Translation(image.left, image.top));
            target_->FillRectangle(visible, checkerBrush_.get());
        } else rect(visible, 0xb8b8b8);
        int step = 1;
        while (step * t->zoom < 0.59f && step < 64) step *= 2;
        auto visibleTiles = [&](int level) {
            const float edge = tileEdge * level * t->zoom;
            return (std::ceil(width(visible) / edge) + 1) * (std::ceil(height(visible) / edge) + 1);
        };
        while (visibleTiles(step) > 460 && step < 64) step *= 2;
        if (step != lod_) { bitmaps_.clear(); lod_ = step; }
        const int edge = step * tileEdge;
        const auto a = screenToImage({visible.left, visible.top});
        const auto b = screenToImage({visible.right, visible.bottom});
        const int x0 = std::max(0, int(a.x / edge)), y0 = std::max(0, int(a.y / edge));
        const int x1 = std::min((state.width - 1) / edge, int(b.x / edge));
        const int y1 = std::min((state.height - 1) / edge, int(b.y / edge));
        for (int ty = y0; ty <= y1; ++ty) for (int tx = x0; tx <= x1; ++tx) {
            const Coord key{tx, ty};
            auto found = bitmaps_.find(key);
            if (found == bitmaps_.end()) {
                cacheTile(intersect({tx * edge, ty * edge, edge, edge}, state.bounds()), step, key);
                found = bitmaps_.find(key);
            }
            if (found == bitmaps_.end()) continue;
            auto& tile = found->second;
            tile.touched = frame_;
            if (tile.image) {
                const auto p = imageToScreen(float(tile.area.x), float(tile.area.y));
                const auto q = imageToScreen(float(tile.area.right()), float(tile.area.bottom()));
                const auto interpolation = t->zoom >= 1.8f ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR :
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
                target_->DrawBitmap(tile.image.get(), Box{p.x, p.y, q.x, q.y}, 1, interpolation);
            }
        }
        target_->PopAxisAlignedClip();
    }
    if (bitmaps_.size() > 480) {
        std::vector<std::pair<std::uint64_t, Coord>> order;
        for (const auto& [key, entry] : bitmaps_) if (entry.touched != frame_) order.emplace_back(entry.touched, key);
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : order) {
            if (bitmaps_.size() <= 480) break;
            bitmaps_.erase(entry.second);
        }
    }
    strokeRect(image, 0x141517);
    auto screen = [&](Rect area) {
        const auto p = imageToScreen(float(area.x), float(area.y));
        return box(p.x, p.y, area.w * t->zoom, area.h * t->zoom);
    };
    const auto selection = drag_ == Drag::marquee ? gesture_ : t->doc.selection;
    if (selection && !selection->empty()) {
        const auto r = screen(*selection);
        strokeRect(r, 0x151618, 1.5f);
        const auto clipped = overlap(r, viewport_);
        for (float x = clipped.left; x < clipped.right; x += 8) {
            line(x, r.top, std::min(x + 4, r.right), r.top, ink);
            line(x, r.bottom, std::min(x + 4, r.right), r.bottom, ink);
        }
        for (float y = clipped.top; y < clipped.bottom; y += 8) {
            line(r.left, y, r.left, std::min(y + 4, r.bottom), ink);
            line(r.right, y, r.right, std::min(y + 4, r.bottom), ink);
        }
    }
    const auto crop = drag_ == Drag::crop ? gesture_ : t->cropBox;
    if (crop && !crop->empty()) {
        const auto r = screen(*crop);
        rect({image.left, image.top, image.right, r.top}, 0x111215, 0.65f);
        rect({image.left, r.bottom, image.right, image.bottom}, 0x111215, 0.65f);
        rect({image.left, r.top, r.left, r.bottom}, 0x111215, 0.65f);
        rect({r.right, r.top, image.right, r.bottom}, 0x111215, 0.65f);
        strokeRect(r, ink);
        for (int i = 1; i < 3; ++i) {
            const float x = r.left + width(r) * i / 3, y = r.top + height(r) * i / 3;
            line(x, r.top, x, r.bottom, ink, 0.7f);
            line(r.left, y, r.right, y, ink, 0.7f);
        }
        for (auto x : {r.left, r.right}) for (auto y : {r.top, r.bottom}) rect(box(x - 3, y - 3, 6, 6), ink);
    }
    if (drag_ == Drag::shape && gesture_ && !gesture_->empty()) {
        const auto r = screen(*gesture_);
        if (tool_ == Tool::ellipse) {
            brush_->SetColor(color(foreground_, 0.6f));
            target_->FillEllipse(D2D1::Ellipse({(r.left + r.right) / 2, (r.top + r.bottom) / 2}, width(r) / 2, height(r) / 2),
                brush_.get());
        } else rect(r, foreground_, 0.6f);
    }
    if (drag_ == Drag::gradient) {
        line(down_.x, down_.y, cursor_.x, cursor_.y, ink);
        circle(down_.x, down_.y, 4, ink); circle(cursor_.x, cursor_.y, 4, ink);
    }
    if (inside(viewport_, cursor_) && inside(image, cursor_) && !space_ &&
        (tool_ == Tool::brush || tool_ == Tool::eraser) && (drag_ == Drag::none || drag_ == Drag::paint)) {
        const float radius = std::max(2.0f, brushSize_ * t->zoom / 2);
        circle(cursor_.x, cursor_.y, radius, 0x151618, false, 2.2f);
        circle(cursor_.x, cursor_.y, radius, 0xf4f4f4, false, 1);
    }
    target_->PopAxisAlignedClip();
}
void App::drawRulers() {
    if (!showRulers_) return;
    const auto t = tab();
    const auto origin = imageToScreen(0, 0);
    const auto horizontal = Box{viewport_.left, tabsBottom, viewport_.right, viewport_.top};
    const auto vertical = Box{viewport_.left - ruler, viewport_.top, viewport_.left, viewport_.bottom};
    rect(horizontal, 0x2d2e31); rect(vertical, 0x2d2e31);
    rect(box(vertical.left, tabsBottom, ruler, ruler), 0x36373a);
    text(box(vertical.left, tabsBottom, ruler, ruler), L"px", muted, 5, center);
    int step = 1;
    constexpr int scale[]{1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
    for (auto n : scale) { step = n; if (n * t->zoom >= 51) break; }
    const int minor = std::max(1, step / 5);
    target_->PushAxisAlignedClip(horizontal, D2D1_ANTIALIAS_MODE_ALIASED);
    const int x0 = int(std::floor((horizontal.left - origin.x) / (t->zoom * minor))) * minor;
    for (int x = x0; origin.x + x * t->zoom < horizontal.right + 3; x += minor) {
        const float p = origin.x + x * t->zoom;
        const bool major = x % step == 0;
        line(p, horizontal.bottom - (major ? 6 : 4), p, horizontal.bottom, faint);
        if (major) text(box(p + 3, horizontal.top - 1, 46, 15), std::to_wstring(x), muted, 5);
    }
    target_->PopAxisAlignedClip();
    target_->PushAxisAlignedClip(vertical, D2D1_ANTIALIAS_MODE_ALIASED);
    const int y0 = int(std::floor((vertical.top - origin.y) / (t->zoom * minor))) * minor;
    for (int y = y0; origin.y + y * t->zoom < vertical.bottom + 3; y += minor) {
        const float p = origin.y + y * t->zoom;
        const bool major = y % step == 0;
        line(vertical.right - (major ? 6 : 4), p, vertical.right, p, faint);
        if (major) {
            target_->SetTransform(D2D1::Matrix3x2F::Rotation(-90, {vertical.left + 7, p - 4}));
            text(box(vertical.left + 7, p - 10, 46, 12), std::to_wstring(y), muted, 5);
            target_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
    }
    target_->PopAxisAlignedClip();
    if (inside(viewport_, cursor_)) {
        line(cursor_.x, horizontal.top, cursor_.x, horizontal.bottom, blueAccent);
        line(vertical.left, cursor_.y, vertical.right, cursor_.y, blueAccent);
    }
    line(horizontal.left, horizontal.bottom - 0.5f, horizontal.right, horizontal.bottom - 0.5f, border);
    line(vertical.right - 0.5f, vertical.top, vertical.right - 0.5f, vertical.bottom, border);
}
void App::drawHome() {
    const float x = (viewport_.left + viewport_.right) / 2;
    const float y = (viewport_.top + viewport_.bottom) / 2 - 75;
    rounded(box(x - 23, y - 40, 46, 43), 9, 0x16304a);
    text(box(x - 23, y - 40, 46, 43), L"Px", blueAccent, 3, center);
    text(box(x - 230, y + 11, 460, 49), L"PixelForge", ink, 4, center);
    text(box(x - 200, y + 63, 400, 23), L"Drop an image here to start editing", muted, 6, center);
    button(box(x - 135, y + 106, 130, 35), L"New Document", Hit::command, int(Command::newDoc), true);
    button(box(x + 5, y + 106, 130, 35), L"Open Image", Hit::command, int(Command::open));
    button(box(x - 65, y + 153, 130, 30), L"Open Sample", Hit::command, int(Command::demo));
}
void App::drawStatus() {
    const float y = h_ - bottomBar;
    rect(box(0, y, w_, bottomBar), menuBg);
    line(0, y + 0.5f, w_, y + 0.5f, border);
    auto t = tab();
    if (!t) {
        text(box(13, y, 185, bottomBar), L"Ready", muted, 2);
        return;
    }
    button(box(9, y + 4, 65, 20), percentage(t->zoom), Hit::zoom, 0, false, L"Set zoom percentage");
    button(box(79, y + 4, 23, 20), L"−", Hit::zoom, -1);
    button(box(104, y + 4, 23, 20), L"+", Hit::zoom, 1);
    line(138, y + 7, 138, y + 21, 0x47484d);
    const auto& d = t->doc.state();
    const auto dimensions = std::to_wstring(d.width) + L" × " + std::to_wstring(d.height) + L" px  ·  RGB / 8";
    text(box(150, y, 200, bottomBar), dimensions, muted, 2);
    if (inside(viewport_, cursor_) && inside(imageRect(), cursor_)) {
        const auto p = imagePoint(cursor_);
        text(box(352, y, 185, bottomBar), L"X " + std::to_wstring(p.x) + L"   Y " + std::to_wstring(p.y), faint, 2);
    }
    text(box(w_ - 235, y, 220, bottomBar), L"Pixel data  " + memory(memory_), muted, 2, right);
}
void App::drawTooltip() {
    if (!tooltip_ || drag_ != Drag::none) return;
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->tip.empty() || !inside(it->rect, cursor_)) continue;
        const float w = std::min(270.0f, 19 + float(it->tip.size()) * 6.4f);
        const float x = std::clamp(cursor_.x + 13, 4.0f, w_ - w - 4);
        const float y = std::clamp(cursor_.y + 19, 4.0f, h_ - 28);
        rounded(box(x + 1, y + 2, w, 24), 4, 0x070809, 0.66f);
        rounded(box(x, y, w, 24), 4, 0x111215);
        text(box(x + 8, y, w - 16, 24), it->tip, ink, 2);
        break;
    }
}
} // namespace pf
