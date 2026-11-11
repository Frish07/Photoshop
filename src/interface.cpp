#include "app.hpp"

#include <cmath>
#include <cwchar>
#include <iomanip>
#include <sstream>

namespace pf {
namespace {
constexpr unsigned ink = 0xededed, muted = 0xbcbcbc, faint = 0x929292, border = 0x1c1c1c;
constexpr unsigned menuBg = 0x292929, panelBg = 0x323232, sub = 0x363636, fieldBg = 0x252525;
constexpr unsigned selected = 0x444c58, blueAccent = 0x79adff, actionBlue = 0x276cde;
constexpr float toolbarWidth = 45, ruler = 20, tabsTop = 70, tabsBottom = 104, bottomBar = 28;
constexpr const wchar_t* labels[]{L"File", L"Edit", L"Image", L"Layer", L"Select", L"Filter", L"View", L"Window", L"Help"};
constexpr float labelWidths[]{37, 39, 54, 49, 51, 46, 45, 66, 46};
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
    const bool chrome = screenMode_ != 2;
    const float sidebarWidth = chrome && showPanels_ && showRightPanels_ && tab() ? std::clamp(w_ * 0.208f, 244.0f, 306.0f) : 0;
    const float toolsWidth = chrome && showPanels_ && tab() ? toolbarWidth : 0;
    sidebar_ = {w_ - sidebarWidth, tabsTop, w_, h_ - bottomBar};
    viewport_ = chrome ? Box{toolsWidth + (showRulers_ ? ruler : 0), tabsBottom + (showRulers_ ? ruler : 0),
        sidebar_.left, h_ - bottomBar} : box(0, 0, w_, h_);
    if (auto t = tab(); t && t->fit) {
        t->zoom = std::clamp(std::min((width(viewport_) - 90) / t->doc.state().width,
            (height(viewport_) - 94) / t->doc.state().height), 0.0125f, 1.0f);
        t->pan = {};
    }
}
void App::paint() {
    if (!createTarget()) return;
    layout();
    const bool animate = selection() && showExtras_ && !IsIconic(window_);
    if (animate && !selectionTimer_) selectionTimer_ = SetTimer(window_, 2, 125, nullptr) != 0;
    else if (!animate && selectionTimer_) { KillTimer(window_, 2); selectionTimer_ = false; }
    ++frame_;
    hits_.clear();
    target_->BeginDraw();
    target_->Clear(color(0x202123));
    try {
        if (screenMode_ != 2) drawHeader();
        if (tab()) {
            drawCanvas();
            if (screenMode_ != 2) {
                drawRulers();
                drawContextBar();
                if (showPanels_) {
                    drawToolbar();
                    if (showRightPanels_) drawSidebar();
                }
            }
        } else drawHome();
        if (screenMode_ != 2) drawStatus();
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
    case Icon::clone:
        l(-2, -1, -2, -5); l(-2, -5, 0, -7); l(0, -7, 2, -5); l(2, -5, 2, -1);
        r(-6, -1, 12, 4); l(-7, 6, 7, 6); break;
    case Icon::compare:
        r(-8, -5, 16, 10); l(0, -8, 0, 8); rect(box(x - 7, y - 4, 6, 8), c, 0.45f); break;
    case Icon::adjustment:
        o(0, 0, 6.5f); l(-4.5f, 4.5f, 4.5f, -4.5f); o(-2.5f, -2.5f, 1.2f, true); break;
    case Icon::transform:
        r(-6, -6, 12, 12);
        for (const int a : {-6, 6}) for (const int b : {-6, 6}) rect(box(x + a - 1.5f, y + b - 1.5f, 3, 3), c);
        break;
    case Icon::lasso:
        l(-3, 5, -7, 3); l(-7, 3, -8, -1); l(-8, -1, -5, -5); l(-5, -5, 1, -6); l(1, -6, 7, -3);
        l(7, -3, 8, 1); l(8, 1, 5, 5); l(5, 5, -1, 5); l(-1, 5, -1, 7); l(-1, 7, 3, 9); break;
    case Icon::polygonLasso:
        l(-7, 2, -4, -6); l(-4, -6, 7, -3); l(7, -3, 5, 6); l(5, 6, -7, 2);
        l(-7, 2, -4, 7); l(-4, 7, 0, 9); break;
    case Icon::wand:
        l(-7, 7, 4, -4); l(-4, 8, 7, -3); l(-7, 7, -4, 8); l(4, -4, 7, -3);
        l(-4, -7, -4, -2); l(-6.5f, -4.5f, -1.5f, -4.5f); l(7, -9, 7, -5); l(5, -7, 9, -7);
        break;
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
    if (!tab()) return;
    rect(box(0, 30, w_, 40), sub);
    line(0, 30.5f, w_, 30.5f, border);
    line(0, 69.5f, w_, 69.5f, border);
    const auto tool = std::find_if(std::begin(tools), std::end(tools), [&](const ToolInfo& t) { return t.tool == tool_; });
    const auto transform = tab() && tab()->transform ? &*tab()->transform : nullptr;
    icon(transform ? Icon::transform : tool->icon, 24, 50, ink);
    text(box(43, 30, 122, 40), transform ? L"Free Transform" : tool->name, ink, 1);
    line(166, 40, 166, 60, 0x53545a);
    x = 180;
    auto option = [&](std::wstring_view label, std::wstring value, Hit kind, int w = 62) {
        const auto labelWidth = float(label.size() * 6 + 14);
        text(box(x, 30, labelWidth, 40), label, muted);
        x += labelWidth;
        button(box(x, 37, float(w), 26), value, kind, 0, false, std::wstring(label));
        x += w + 17;
    };
    if (transform) {
        option(L"W × H", std::to_wstring(transform->box.w) + L" × " + std::to_wstring(transform->box.h), Hit::transformSize, 110);
        button(box(x, 37, 65, 26), L"Link W/H", Hit::transformLink, 0, transform->linked, L"Maintain aspect ratio");
        x += 76;
        option(L"Angle", std::to_wstring(int(std::round(transform->degrees))) + L"°", Hit::transformAngle, 48);
        iconButton(box(x, 37, 31, 26), Icon::check, Hit::command, int(Command::applyTransform), true, L"Commit transform (Enter)");
        iconButton(box(x + 36, 37, 31, 26), Icon::close, Hit::command, int(Command::cancelTransform), false, L"Cancel transform (Esc)");
    } else if (tool_ == Tool::brush || tool_ == Tool::eraser || tool_ == Tool::clone) {
        option(L"Size", std::to_wstring(brushSize_) + L" px", Hit::fgSize);
        option(L"Hardness", std::to_wstring(hardness_) + L"%", Hit::hardness);
        option(L"Opacity", std::to_wstring(flow_) + L"%", Hit::flow);
        if (tool_ == Tool::clone) {
            button(box(x, 37, 112, 26), cloneMerged_ ? L"All Layers" : L"Current Layer", Hit::cloneSample, 0, true,
                L"Choose which layers to sample");
        } else if (w_ > 930) {
            circle(x + 3, 50, 7, tool_ == Tool::brush ? foreground_ : muted, tool_ == Tool::brush);
            text(box(x + 18, 30, 120, 40), tool_ == Tool::brush ? L"Soft Round" : L"Round Eraser", faint);
        }
    } else if (selectionTool(tool_)) {
        const wchar_t* signs[]{L"□", L"+", L"−", L"∩"};
        const wchar_t* tips[]{L"New Selection", L"Add to Selection (Shift)", L"Subtract from Selection (Alt)", L"Intersect (Shift+Alt)"};
        for (int i = 0; i < 4; ++i) {
            button(box(x, 37, 27, 26), signs[i], Hit::selectionMode, i, int(selectionMode_) == i, tips[i]);
            x += 30;
        }
        x += 8;
        if (tool_ == Tool::wand) {
            text(box(x, 30, 63, 40), L"Tolerance", muted);
            x += 63;
            button(box(x, 37, 42, 26), std::to_wstring(wandTolerance_), Hit::tolerance, 1, false, L"Magic Wand tolerance");
            x += 49;
            button(box(x, 37, 80, 26), L"Contiguous", Hit::wandContiguous, 0, wandContiguous_);
            x += 86;
            button(box(x, 37, 100, 26), L"Sample All", Hit::wandMerged, 0, wandMerged_);
        } else {
            const auto hint = tool_ == Tool::polygonLasso ? L"Click corners · Enter to close" :
                tool_ == Tool::lasso ? L"Draw a freehand selection" : L"Hold Shift during drag to constrain";
            text(box(x, 30, 230, 40), hint, muted);
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
    } else if (tool_ == Tool::move) {
        button(box(x, 37, 101, 26), L"Auto-Select", Hit::autoSelect, 0, autoSelect_, L"Pick the layer under the cursor (Ctrl toggles)");
        button(box(x + 110, 37, 99, 26), L"Free Transform", Hit::command, int(Command::freeTransform));
    } else {
        const wchar_t* hint = tool_ == Tool::picker ? L"Sample all visible layers" :
            tool_ == Tool::gradient ? L"Linear · Foreground to Background" :
            tool_ == Tool::hand ? L"Drag to pan" : L"Drag to draw · Shift to constrain";
        text(box(x, 30, 290, 40), hint, muted);
    }
    if (tab()) {
        iconButton(box(w_ - 151, 36, 32, 27), Icon::compare, Hit::command, int(Command::compare), tab()->compare,
            L"Quick Compare (\\)");
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
    for (std::size_t i = 0; i < std::size(tools);) {
        const auto first = i;
        const auto key = tools[i++].key;
        while (i < std::size(tools) && tools[i].key == key) ++i;
        const auto prefer = preferredTools_[key - L'A'];
        const auto chosen = prefer ? std::find_if(tools + first, tools + i, [&](const ToolInfo& tool) { return tool.tool == *prefer; }) :
            tools + first;
        const auto& t = chosen == tools + i ? tools[first] : *chosen;
        if (y + 77 > area.bottom) break;
        if (key == L'I' || key == L'T' || key == L'H') { line(9, y + 2, 36, y + 2, 0x454545); y += 8; }
        const auto r = box(6, y, 33, 29);
        const auto label = std::wstring(t.name) + L" (" + t.key + L")";
        iconButton(r, t.icon, Hit::tool, int(t.tool), t.tool == tool_, label);
        if (i - first > 1) {
            line(r.right - 7, r.bottom - 5, r.right - 4, r.bottom - 5, muted);
            line(r.right - 4, r.bottom - 8, r.right - 4, r.bottom - 5, muted);
            hit(box(r.right - 9, r.bottom - 10, 9, 10), Hit::moreTools, key, L"More tools in this group");
        }
        if (t.tool == tool_) rect(box(6, y + 5, 2, 19), blueAccent);
        y += 30;
    }
    iconButton(box(6, y + 2, 33, 25), Icon::more, Hit::moreTools, 0, false, L"All Tools");
    y = std::min(y + 44, area.bottom - 43);
    strokeRect(box(23, y + 8, 18, 18), border);
    rect(box(24, y + 9, 16, 16), background_);
    hit(box(29, y + 17, 13, 12), Hit::color, 1, L"Background color");
    strokeRect(box(11, y - 2, 22, 22), 0x9a9ba0);
    rect(box(12, y - 1, 20, 20), foreground_);
    hit(box(11, y - 2, 22, 22), Hit::color, 0, L"Foreground color");
    icon(Icon::swap, 36, y - 4, muted);
    hit(box(30, y - 13, 14, 15), Hit::swap, 0, L"Swap Colors (X)");
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
    const wchar_t* names[]{L"Color", L"Swatches", L"Properties"};
    for (int i = 0; i < int(std::size(names)); ++i) {
        const auto r = box(area.left + i * 76, area.top, 76, 34);
        if (i == colorTab_) { rect(r, panelBg); rect(box(r.left + 13, r.bottom - 2, 50, 2), blueAccent); }
        else if (inside(r, cursor_)) rect(r, 0x3b3d41);
        text(r, names[i], i == colorTab_ ? ink : muted, i == colorTab_ ? 1 : 0, center);
        hit(r, Hit::colorTab, i);
    }
    if (colorTab_ == 2) {
        drawProperties({area.left, area.top + 34, area.right, area.bottom});
        return;
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
void App::drawProperties(Box area) {
    const auto& state = tab()->doc.state();
    const auto& layer = tab()->doc.active();
    const float x = area.left + 13, top = area.top + 10, w = width(area) - 26;
    if (layer.adjustment) {
        const auto a = *layer.adjustment;
        const auto name = a.kind == AdjustmentKind::tone ? L"Brightness/Contrast" :
            a.kind == AdjustmentKind::levels ? L"Levels" : L"Hue/Saturation";
        text(box(x, top, w, 26), name, ink, 1);
        auto entry = [&](int row, const wchar_t* label, int number) {
            text(box(x, top + 28 + row * 21, w, 21), label, muted, 0);
            text(box(x, top + 28 + row * 21, w, 21), std::to_wstring(number), ink, 0, right);
        };
        if (a.kind == AdjustmentKind::levels) {
            entry(0, L"Black point", a.first); entry(1, L"White point", a.second); entry(2, L"Gamma × 100", a.third);
        } else if (a.kind == AdjustmentKind::hueSaturation) {
            entry(0, L"Hue", a.first); entry(1, L"Saturation", a.second); entry(2, L"Lightness", a.third);
        } else {
            entry(0, L"Brightness", a.first); entry(1, L"Contrast", a.second);
        }
        button(box(x, top + 100, w, 27), L"Edit Adjustment", Hit::command, int(Command::editAdjustment), true);
        if (height(area) >= 176) text(box(x, top + 137, w, 22), L"Paint the mask to limit the effect", faint, 2);
        return;
    }
    text(box(x, top, w, 24), L"Canvas", ink, 1);
    button(box(x, top + 28, w * 0.56f, 26), std::to_wstring(state.width) + L" × " + std::to_wstring(state.height) + L" px",
        Hit::command, int(Command::canvasSize));
    button(box(x + w * 0.56f + 5, top + 28, w * 0.44f - 5, 26), std::to_wstring(int(std::round(state.ppi))) + L" PPI",
        Hit::command, int(Command::resolution));
    line(x, top + 66, x + w, top + 66, 0x474747);
    text(box(x, top + 71, w, 26), L"Add Adjustment Layer", ink, 1);
    button(box(x, top + 101, w, 24), L"Brightness/Contrast", Hit::command, int(Command::addTone));
    if (height(area) >= 166) {
        button(box(x, top + 130, w * 0.42f, 24), L"Levels", Hit::command, int(Command::addLevels));
        button(box(x + w * 0.42f + 5, top + 130, w * 0.58f - 5, 24), L"Hue/Saturation", Hit::command, int(Command::addHue));
    }
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
            const auto bounds = layer.pixels.tiles().empty() ? Rect{-layer.x, -layer.y,
                tab()->doc.state().width, tab()->doc.state().height} : layer.pixels.extent();
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
    text(box(x + 9, y, width(blend) - 27, 26), blendName(layer.blend), ink);
    icon(Icon::down, blend.right - 12, y + 13, muted);
    hit(blend, Hit::blend, 0, L"Layer blending mode");
    const auto op = box(blend.right + 10, y, area.right - 12 - blend.right - 10, 26);
    rounded(op, 4, inside(op, cursor_) ? 0x45474d : fieldBg);
    text(box(op.left + 8, y, 52, 26), L"Opacity", muted, 2);
    text(box(op.right - 44, y, 36, 26), std::to_wstring((layer.opacity * 100 + 127) / 255) + L"%", ink, 0, right);
    hit(op, Hit::opacity, 0, L"Change layer opacity");
    text(box(x + 2, y + 31, 37, 24), L"Lock", muted, 2);
    iconButton(box(x + 36, y + 31, 24, 24), Icon::lock, Hit::command, int(Command::toggleLock), layer.locked, L"Lock or unlock layer");
    if (!layer.adjustment) {
        const auto fill = box(op.left, y + 31, width(op), 24);
        rounded(fill, 4, inside(fill, cursor_) ? 0x45474d : fieldBg);
        text(box(fill.left + 8, fill.top, 44, 24), L"Fill", muted, 2);
        text(box(fill.right - 45, fill.top, 37, 24), std::to_wstring((layer.fillOpacity * 100 + 127) / 255) + L"%", ink, 0, right);
        hit(fill, Hit::fillOpacity, 0, L"Change fill opacity");
    }
    const auto rowTop = area.top + 73;
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
        if (l.adjustment) {
            rect(preview, 0x242424);
            icon(Icon::adjustment, preview.left + 18, preview.top + 14, ink);
        } else thumb(preview, l, false);
        strokeRect(preview, active && !t->mask ? blueAccent : 0x202124);
        hit(preview, Hit::layerThumbnail, index, L"Ctrl-click to load layer transparency");
        float tx = preview.right + 10;
        if (l.mask) {
            const auto mask = box(preview.right + 5, preview.top, 27, 29);
            thumb(mask, l, true);
            strokeRect(mask, active && t->mask ? blueAccent : 0x202124);
            if (!l.maskEnabled) {
                line(mask.left + 4, mask.top + 4, mask.right - 4, mask.bottom - 4, 0xfa5252, 2);
                line(mask.right - 4, mask.top + 4, mask.left + 4, mask.bottom - 4, 0xfa5252, 2);
            }
            hit(mask, Hit::mask, index, L"Select mask · Shift-click toggles its effect");
            tx = mask.right + 10;
        }
        const bool styled = !l.style.empty();
        text(box(tx, r.top, r.right - tx - (styled ? 61 : 37), 45), l.name, active ? ink : muted, active ? 1 : 0);
        if (styled) {
            const auto fx = box(r.right - 58, r.top + 10, 26, 25);
            text(fx, L"fx", blueAccent, 1, center);
            hit(fx, Hit::styles, index, L"Layer effects");
        }
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
    text(box(area.left + 10, footer, 73, 35), std::to_wstring(state.layers.size()) +
        (state.layers.size() == 1 ? L" layer" : L" layers"), faint, 2);
    const std::array icons{Icon::down, Icon::up, Icon::mask, Icon::plus, Icon::trash};
    const std::array actions{Command::layerDown, Command::layerUp, Command::addMask, Command::addLayer, Command::deleteLayer};
    const wchar_t* tips[]{L"Send Backward", L"Bring Forward", L"Add Layer Mask", L"New Layer (Ctrl+Shift+N)", L"Delete Layer"};
    const auto fx = box(area.right - 158, footer + 5, 25, 25);
    if (inside(fx, cursor_)) rounded(fx, 4, 0x444444);
    text(fx, L"fx", layer.style.empty() ? ink : blueAccent, 1, center);
    hit(fx, Hit::styles, -1, L"Add a layer style");
    for (int i = 0; i < 5; ++i) iconButton(box(area.right - 132 + i * 24, footer + 5, 23, 25), icons[i], Hit::command,
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
void App::cacheTile(const State& state, Rect area, int step, Coord key, BitmapCache& cache) {
    const int w = (area.w + step - 1) / step, h = (area.h + step - 1) / step;
    std::array<Pixel, tileEdge * tileEdge> buffer;
    auto pixels = std::span(buffer).first(std::size_t(w) * h);
    std::optional<TransformView> transform;
    if (const auto& t = tab()->transform; &cache == &bitmaps_ && t && (t->box != t->original || t->degrees))
        transform = TransformView{t->original, t->box, t->degrees};
    preview(state, area, step, w, h, pixels, transform ? &*transform : nullptr);
    Bitmap entry{area, {}, frame_};
    if (std::any_of(pixels.begin(), pixels.end(), [](Pixel p) { return p != 0; })) {
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(target_->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, props, entry.image.put()))) return;
    }
    cache.emplace(key, std::move(entry));
}
void App::drawTiles(const State& state, Box visible, int step, BitmapCache& cache) {
    if (width(visible) <= 0 || height(visible) <= 0) return;
    target_->PushAxisAlignedClip(visible, D2D1_ANTIALIAS_MODE_ALIASED);
    const int edge = step * tileEdge;
    const auto a = screenToImage({visible.left, visible.top});
    const auto b = screenToImage({visible.right, visible.bottom});
    const int x0 = std::max(0, int(a.x / edge)), y0 = std::max(0, int(a.y / edge));
    const int x1 = std::min((state.width - 1) / edge, int(b.x / edge));
    const int y1 = std::min((state.height - 1) / edge, int(b.y / edge));
    const auto interpolation = tab()->zoom >= 1.8f ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR :
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
    for (int ty = y0; ty <= y1; ++ty) for (int tx = x0; tx <= x1; ++tx) {
        const Coord key{tx, ty};
        auto found = cache.find(key);
        if (found == cache.end()) {
            cacheTile(state, intersect({tx * edge, ty * edge, edge, edge}, state.bounds()), step, key, cache);
            found = cache.find(key);
        }
        if (found == cache.end()) continue;
        auto& tile = found->second;
        tile.touched = frame_;
        if (tile.image) {
            const auto p = imageToScreen(float(tile.area.x), float(tile.area.y));
            const auto q = imageToScreen(float(tile.area.right()), float(tile.area.bottom()));
            target_->DrawBitmap(tile.image.get(), Box{p.x, p.y, q.x, q.y}, 1, interpolation);
        }
    }
    target_->PopAxisAlignedClip();
}
void App::drawCanvas() {
    auto t = tab();
    const auto& state = t->doc.state();
    target_->PushAxisAlignedClip(viewport_, D2D1_ANTIALIAS_MODE_ALIASED);
    rect(viewport_, 0x202020);
    const auto image = imageRect();
    for (int i = 9; i > 0; --i) rect(box(image.left - i / 2.0f, image.top + 2 - i / 4.0f,
        width(image) + i, height(image) + i), 0x070809, 0.035f);
    const auto visible = overlap(viewport_, image);
    if (width(visible) > 0 && height(visible) > 0) {
        if (!checkerBrush_) {
            std::array<Pixel, 20 * 20> pattern;
            for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x)
                pattern[y * 20 + x] = (x < 10) == (y < 10) ? 0xffb8b8b8 : 0xff939396;
            const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (SUCCEEDED(target_->CreateBitmap(D2D1::SizeU(20, 20), pattern.data(), 80, props, checker_.put())))
                target_->CreateBitmapBrush(checker_.get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,
                    D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), checkerBrush_.put());
        }
        auto checker = [&](Box area) {
            if (width(area) <= 0 || height(area) <= 0) return;
            if (checkerBrush_) {
                checkerBrush_->SetTransform(D2D1::Matrix3x2F::Translation(image.left, image.top));
                target_->FillRectangle(area, checkerBrush_.get());
            } else rect(area, 0xb8b8b8);
        };
        int step = 1;
        while (step * t->zoom < 0.59f && step < 64) step *= 2;
        auto visibleTiles = [&](int level) {
            const float edge = tileEdge * level * t->zoom;
            return (std::ceil(width(visible) / edge) + 1) * (std::ceil(height(visible) / edge) + 1);
        };
        while (visibleTiles(step) > (t->compare ? 220 : 460) && step < 64) step *= 2;
        while (t->transform && width(visible) * height(visible) / std::pow(t->zoom * step, 2) > 460000 && step < 64) step *= 2;
        if (step != lod_) { bitmaps_.clear(); beforeBitmaps_.clear(); lod_ = step; }
        if (t->compare) {
            const float divider = visible.left + width(visible) * t->split;
            const Box after{divider, visible.top, visible.right, visible.bottom};
            const auto& original = t->doc.historyStart();
            const auto end = imageToScreen(float(original.width), float(original.height));
            const Box before = overlap({visible.left, visible.top, divider, visible.bottom}, {image.left, image.top, end.x, end.y});
            checker(before);
            drawTiles(original, before, step, beforeBitmaps_);
            checker(after);
            drawTiles(state, after, step, bitmaps_);
            line(divider, visible.top, divider, visible.bottom, 0x171717, 3);
            line(divider, visible.top, divider, visible.bottom, ink, 1.4f);
            const float y = (visible.top + visible.bottom) / 2;
            rounded(box(divider - 15, y - 12, 30, 24), 11, 0x282828);
            icon(Icon::swap, divider, y, ink);
            hit(box(divider - 7, visible.top, 14, height(visible)), Hit::compareSlider, 0, L"Drag to compare before and after");
            hit(box(divider - 16, y - 14, 32, 28), Hit::compareSlider);
            if (width(visible) > 260 && height(visible) > 58) {
                const bool first = t->doc.historyStartId() == 0;
                const float w = first ? 55 : 91;
                if (divider - visible.left > w + 16) {
                    const auto label = box(visible.left + 8, visible.top + 8, w, 22);
                    rounded(label, 5, 0x222222, 0.88f);
                    text(label, first ? L"Before" : L"History Start", ink, 2, center);
                }
                if (visible.right - divider > 64) {
                    const auto label = box(visible.right - 57, visible.top + 8, 49, 22);
                    rounded(label, 5, 0x222222, 0.88f);
                    text(label, L"After", ink, 2, center);
                }
            }
        } else {
            checker(visible);
            drawTiles(state, visible, step, bitmaps_);
        }
    }
    auto trim = [&](BitmapCache& cache, std::size_t limit) {
        if (cache.size() <= limit) return;
        std::vector<std::pair<std::uint64_t, Coord>> order;
        for (const auto& [key, entry] : cache) if (entry.touched != frame_) order.emplace_back(entry.touched, key);
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& entry : order) {
            if (cache.size() <= limit) break;
            cache.erase(entry.second);
        }
    };
    trim(bitmaps_, t->compare ? 240 : 480);
    trim(beforeBitmaps_, 240);
    strokeRect(image, 0x141517);
    auto screen = [&](Rect area) {
        const auto p = imageToScreen(float(area.x), float(area.y));
        return box(p.x, p.y, area.w * t->zoom, area.h * t->zoom);
    };
    drawGuides();
    drawSelection();
    drawSelectionGesture();
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
    if (tool_ == Tool::clone && t->cloneSource && !t->transform) {
        const auto anchor = *t->cloneSource;
        const auto pos = drag_ == Drag::paint ? imagePoint(cursor_) : anchor;
        const auto offset = drag_ == Drag::paint ? cloneOffset_ : Point{};
        const auto p = imageToScreen(pos.x + offset.x + 0.5f, pos.y + offset.y + 0.5f);
        line(p.x - 7, p.y, p.x + 7, p.y, 0x151515, 2.8f);
        line(p.x, p.y - 7, p.x, p.y + 7, 0x151515, 2.8f);
        line(p.x - 7, p.y, p.x + 7, p.y, ink);
        line(p.x, p.y - 7, p.x, p.y + 7, ink);
    }
    if (t->transform) drawTransform();
    if (!t->transform && inside(viewport_, cursor_) && inside(image, cursor_) && !space_ &&
        (tool_ == Tool::brush || tool_ == Tool::eraser || tool_ == Tool::clone) && (drag_ == Drag::none || drag_ == Drag::paint)) {
        const float radius = std::max(2.0f, brushSize_ * t->zoom / 2);
        circle(cursor_.x, cursor_.y, radius, 0x151618, false, 2.2f);
        circle(cursor_.x, cursor_.y, radius, 0xf4f4f4, false, 1);
    }
    target_->PopAxisAlignedClip();
}
void App::drawSelection() {
    const auto selected = selection();
    if (!selected) {
        selectionDrawn_.reset();
        selectionPath_.reset();
        return;
    }
    if (!showExtras_ || (gestureMode_ == SelectMode::replace && (drag_ == Drag::marquee || drag_ == Drag::lasso))) return;
    if (!selectionDrawn_ || *selectionDrawn_ != *selected) {
        selectionDrawn_ = *selected;
        if (FAILED(factory_->CreatePathGeometry(selectionPath_.put()))) return;
        Com<ID2D1GeometrySink> sink;
        if (FAILED(selectionPath_->Open(sink.put()))) { selectionPath_.reset(); return; }
        const auto edges = selected->outline();
        std::unordered_multimap<Coord, std::size_t, CoordHash> adjacent;
        adjacent.reserve(edges.size() * 2);
        for (std::size_t i = 0; i < edges.size(); ++i) {
            adjacent.emplace(Coord{edges[i].from.x, edges[i].from.y}, i);
            adjacent.emplace(Coord{edges[i].to.x, edges[i].to.y}, i);
        }
        std::vector<bool> drawn(edges.size());
        for (std::size_t i = 0; i < edges.size(); ++i) {
            if (drawn[i]) continue;
            const auto start = edges[i].from;
            auto current = edges[i].to;
            sink->BeginFigure({float(start.x), float(start.y)}, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine({float(current.x), float(current.y)});
            drawn[i] = true;
            while (current != start) {
                const auto [first, last] = adjacent.equal_range({current.x, current.y});
                const auto next = std::find_if(first, last, [&](const auto& edge) { return !drawn[edge.second]; });
                if (next == last) break;
                const auto& edge = edges[next->second];
                drawn[next->second] = true;
                current = current == edge.from ? edge.to : edge.from;
                sink->AddLine({float(current.x), float(current.y)});
            }
            sink->EndFigure(current == start ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        }
        if (FAILED(sink->Close())) { selectionPath_.reset(); return; }
    }
    if (!selectionPath_) return;
    const auto phase = std::size_t((GetTickCount64() / 125) % marchingStyles_.size());
    auto& style = marchingStyles_[phase];
    if (!style) {
        auto properties = D2D1::StrokeStyleProperties();
        properties.dashStyle = D2D1_DASH_STYLE_CUSTOM;
        properties.dashOffset = float(phase * 2);
        const float dashes[]{4, 4};
        factory_->CreateStrokeStyle(properties, dashes, 2, style.put());
    }
    const auto origin = imageToScreen(0, 0);
    const auto zoom = tab()->zoom;
    target_->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom) * D2D1::Matrix3x2F::Translation(origin.x, origin.y));
    brush_->SetColor(color(0x111111));
    target_->DrawGeometry(selectionPath_.get(), brush_.get(), 1.65f / zoom);
    if (style) {
        brush_->SetColor(color(0xf7f7f7));
        target_->DrawGeometry(selectionPath_.get(), brush_.get(), 1.0f / zoom, style.get());
    }
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
}
void App::drawSelectionGesture() {
    if (drag_ == Drag::marquee && gesture_) {
        const auto a = imageToScreen(float(gesture_->x), float(gesture_->y));
        const auto b = imageToScreen(float(gesture_->right()), float(gesture_->bottom()));
        const Box area{a.x, a.y, b.x, b.y};
        if (tool_ == Tool::ellipticalMarquee) {
            const auto outline = D2D1::Ellipse({(a.x + b.x) / 2, (a.y + b.y) / 2}, width(area) / 2, height(area) / 2);
            brush_->SetColor(color(0x101010));
            target_->DrawEllipse(outline, brush_.get(), 2.5f);
            brush_->SetColor(color(ink));
            target_->DrawEllipse(outline, brush_.get(), 1);
        } else {
            strokeRect(area, 0x101010, 2.5f);
            strokeRect(area, ink);
        }
    }
    if (lassoPath_.empty()) return;
    auto toScreen = [&](Point p) { return imageToScreen(float(p.x), float(p.y)); };
    auto segment = [&](Vec a, Vec b) {
        line(a.x, a.y, b.x, b.y, 0x111111, 2.4f);
        line(a.x, a.y, b.x, b.y, ink);
    };
    for (std::size_t i = 1; i < lassoPath_.size(); ++i) segment(toScreen(lassoPath_[i - 1]), toScreen(lassoPath_[i]));
    if (tool_ == Tool::polygonLasso) {
        if (inside(viewport_, cursor_)) segment(toScreen(lassoPath_.back()), cursor_);
        const auto first = toScreen(lassoPath_.front());
        circle(first.x, first.y, 3.5f, 0x101010, true);
        circle(first.x, first.y, 3, ink);
    }
}
void App::drawTransform() {
    const auto handles = transformHandles();
    for (int i = 0; i < 8; i += 2) {
        const auto a = handles[i], b = handles[(i + 2) % 8];
        line(a.x, a.y, b.x, b.y, 0x202020, 2.6f);
        line(a.x, a.y, b.x, b.y, blueAccent, 1.2f);
    }
    for (int i = 0; i < 8; ++i) {
        const auto p = handles[i];
        const auto handle = box(p.x - 3.5f, p.y - 3.5f, 7, 7);
        rect(handle, ink);
        strokeRect(handle, actionBlue);
    }
    line(handles[1].x, handles[1].y, handles[8].x, handles[8].y, blueAccent, 1.4f);
    circle(handles[8].x, handles[8].y, 4.5f, ink, true);
    circle(handles[8].x, handles[8].y, 4.5f, actionBlue);
}
void App::drawContextBar() {
    const auto t = tab();
    if (!showContextBar_ || !showPanels_ || width(viewport_) < 385 || drag_ != Drag::none) return;
    struct Action { const wchar_t* name; Command command; };
    std::array<Action, 3> actions;
    if (t->transform) actions = {{{L"Apply Transform", Command::applyTransform}, {L"Cancel", Command::cancelTransform}, {L"", Command::none}}};
    else if (t->cropBox) actions = {{{L"Apply Crop", Command::cropSelection}, {L"Cancel", Command::deselect}, {L"", Command::none}}};
    else if (t->doc.selection) actions = {{{L"Layer via Copy", Command::duplicate}, {L"Inverse", Command::inverseSelection}, {L"Deselect", Command::deselect}}};
    else if (t->doc.active().adjustment) actions = {{{L"Edit Adjustment", Command::editAdjustment}, {L"New Layer", Command::addLayer}, {L"", Command::none}}};
    else if (t->doc.active().locked) actions = {{{L"Unlock Layer", Command::toggleLock}, {L"New Layer", Command::addLayer}, {L"Place Image", Command::place}}};
    else actions = {{{L"New Layer", Command::addLayer}, {L"Place Image", Command::place}, {L"Add Adjustment", Command::addTone}}};
    float w = 16;
    for (auto a : actions) if (a.command != Command::none) w += 22 + float(std::wcslen(a.name)) * 6.5f;
    const auto image = imageRect();
    const float x = (viewport_.left + viewport_.right - w) / 2;
    const float y = std::clamp(image.bottom + 11, viewport_.top + 12, viewport_.bottom - 48);
    rounded(box(x + 1, y + 2, w, 36), 7, 0x0a0a0a, 0.65f);
    rounded(box(x, y, w, 36), 7, 0x373737);
    float left = x + 8;
    for (auto a : actions) if (a.command != Command::none) {
        const float bw = 17 + float(std::wcslen(a.name)) * 6.5f;
        button(box(left, y + 6, bw, 24), a.name, Hit::command, int(a.command));
        left += bw + 5;
    }
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
    hit(horizontal, Hit::ruler, 0, L"Drag out a horizontal guide");
    hit(vertical, Hit::ruler, 1, L"Drag out a vertical guide");
}
void App::drawGuides() {
    if (!showExtras_ || !showGuides_) return;
    const auto image = imageRect();
    auto draw = [&](Guide guide, bool dragging) {
        const auto p = guide.vertical ? imageToScreen(float(guide.position), 0) : imageToScreen(0, float(guide.position));
        const auto c = dragging ? 0xa3f3ffu : 0x4fd9f3u;
        if (guide.vertical) line(p.x, image.top, p.x, image.bottom, c, dragging ? 1.5f : 1);
        else line(image.left, p.y, image.right, p.y, c, dragging ? 1.5f : 1);
    };
    const auto& guides = tab()->doc.state().guides;
    for (std::size_t i = 0; i < guides.size(); ++i) {
        if (drag_ == Drag::guide && movingGuide_ && int(i) == *movingGuide_) continue;
        draw(guides[i], false);
    }
    if (drag_ == Drag::guide) draw(guideDraft_, true);
}
void App::drawHome() {
    constexpr float rail = 202;
    rect(box(0, 30, rail, h_ - 30 - bottomBar), 0x292929);
    line(rail, 30, rail, h_ - bottomBar, border);
    const auto create = box(22, 62, rail - 44, 35);
    rounded(create, 17, inside(create, cursor_) ? 0x377de8 : actionBlue);
    text(create, L"New File", ink, 1, center);
    hit(create, Hit::command, int(Command::newDoc), L"New Document (Ctrl+N)");
    button(box(22, 109, rail - 44, 34), L"Open", Hit::command, int(Command::open), false, L"Open Image (Ctrl+O)");
    const float x = (w_ + rail) / 2, y = (h_ - bottomBar + 30) / 2 - 54;
    icon(Icon::image, x, y, blueAccent);
    text(box(x - 220, y + 19, 440, 46), L"Start a new document", ink, 4, center);
    text(box(x - 245, y + 72, 490, 27), L"Choose a preset or drop an image here to open it.", muted, 6, center);
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
    const auto dimensions = std::to_wstring(d.width) + L" × " + std::to_wstring(d.height) + L" px  ·  " +
        std::to_wstring(int(std::round(d.ppi))) + L" PPI  ·  RGB / 8";
    text(box(150, y, 238, bottomBar), dimensions, muted, 2);
    if (inside(viewport_, cursor_) && inside(imageRect(), cursor_)) {
        const auto p = imagePoint(cursor_);
        text(box(397, y, 182, bottomBar), L"X " + std::to_wstring(p.x) + L"   Y " + std::to_wstring(p.y), faint, 2);
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
