#include "app.hpp"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace pf {
namespace {
constexpr std::array<Point, 8> directions{{{-1, -1}, {0, -1}, {1, -1}, {1, 0},
    {1, 1}, {0, 1}, {-1, 1}, {-1, 0}}};

std::wstring adjustmentName(AdjustmentKind kind) {
    switch (kind) {
    case AdjustmentKind::tone: return L"Brightness/Contrast";
    case AdjustmentKind::levels: return L"Levels";
    case AdjustmentKind::hueSaturation: return L"Hue/Saturation";
    }
    return {};
}

std::vector<Field> fieldsFor(Adjustment a) {
    if (a.kind == AdjustmentKind::levels) return {{L"Black point (0–254)", std::to_wstring(a.first)},
        {L"White point (1–255)", std::to_wstring(a.second)}, {L"Gamma × 100 (10–1000)", std::to_wstring(a.third)}};
    if (a.kind == AdjustmentKind::hueSaturation) return {{L"Hue (−180° to 180°)", std::to_wstring(a.first)},
        {L"Saturation (−100 to 100)", std::to_wstring(a.second)}, {L"Lightness (−100 to 100)", std::to_wstring(a.third)}};
    return {{L"Brightness (−100 to 100)", std::to_wstring(a.first)}, {L"Contrast (−100 to 100)", std::to_wstring(a.second)}};
}
std::wstring hexColor(Pixel color) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result = L"#000000";
    for (int i = 0; i < 6; ++i) result[6 - i] = digits[(color >> (4 * i)) & 15];
    return result;
}
Pixel effectColor(const std::wstring& input, int opacity) {
    const auto color = input.starts_with(L'#') ? input.substr(1) : input;
    if (color.size() != 6 || color.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos)
        throw std::runtime_error("Enter a six-digit RGB color, such as #203040.");
    return Pixel(std::stoul(color, nullptr, 16)) | (Pixel((opacity * 255 + 50) / 100) << 24);
}
} // namespace

void App::createDocument() {
    const auto options = newDocumentDialog(window_, L"Untitled-" + std::to_wstring(created_ + 1), background_);
    if (!options) return;
    if (options->openExisting) { command(Command::open); return; }
    ++created_;
    addTab(blankDocument(options->width, options->height, options->ppi, options->background), options->name, L"New Document");
}

void App::setScreenMode(int mode) {
    if (screenMode_ == mode) return;
    if (screenMode_ == 0) {
        standardPlacement_.length = sizeof(standardPlacement_);
        if (!GetWindowPlacement(window_, &standardPlacement_)) return;
    }
    screenMode_ = mode;
    if (mode == 0) {
        SetWindowLongPtrW(window_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(window_, &standardPlacement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        const auto rect = monitor.rcMonitor;
        SetWindowLongPtrW(window_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window_, HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_FRAMECHANGED);
    }
    repaint();
}

std::optional<int> App::nearestGuide(Vec p) const {
    if (guidesLocked_ || !showGuides_ || !showExtras_ || !tab() || !inside(imageRect(), p)) return std::nullopt;
    const auto& guides = tab()->doc.state().guides;
    std::optional<int> result;
    float distance = 5;
    for (std::size_t i = 0; i < guides.size(); ++i) {
        const auto guide = guides[i];
        const auto target = guide.vertical ? imageToScreen(float(guide.position), 0).x : imageToScreen(0, float(guide.position)).y;
        const float delta = std::abs(target - (guide.vertical ? p.x : p.y));
        if (delta < distance) { distance = delta; result = int(i); }
    }
    return result;
}

void App::startGuide(bool vertical, std::optional<int> existing, Vec p) {
    if (!tab() || guidesLocked_) return;
    if (!existing && tab()->doc.state().guides.size() >= maxGuides) { MessageBeep(MB_ICONWARNING); return; }
    const auto location = screenToImage(p);
    guideDraft_ = {vertical, int(std::round(vertical ? location.x : location.y))};
    movingGuide_ = existing;
    showGuides_ = true;
    showExtras_ = true;
    drag_ = Drag::guide;
    down_ = last_ = p;
    SetCapture(window_);
    repaint();
}

void App::finishGuide(Vec p) {
    auto& doc = tab()->doc;
    auto& state = doc.edit();
    const bool place = inside(viewport_, p) && guideDraft_.position >= 0 &&
        guideDraft_.position <= (guideDraft_.vertical ? state.width : state.height);
    if (!place && !movingGuide_) return;
    if (movingGuide_ && *movingGuide_ >= int(state.guides.size())) return;
    doc.begin();
    if (movingGuide_) state.guides.erase(state.guides.begin() + *movingGuide_);
    if (place && std::find(state.guides.begin(), state.guides.end(), guideDraft_) == state.guides.end())
        state.guides.push_back(guideDraft_);
    doc.commit(place ? L"Position Guide" : L"Remove Guide");
    documentChanged(false);
}

Point App::snappedMove(Rect from, Point offset) const {
    if (!snap_ || !tab() || (GetKeyState(VK_CONTROL) & 0x8000)) return offset;
    const auto& state = tab()->doc.state();
    const int threshold = int(std::ceil(6.0f / tab()->zoom));
    auto snapAxis = [&](int position, int extent, int delta, bool vertical, int span) {
        const std::array edges{position + delta, position + delta + extent / 2, position + delta + extent};
        int correction = 0, distance = threshold + 1;
        auto consider = [&](int target) {
            for (const int edge : edges) {
                const int candidate = target - edge;
                if (std::abs(candidate) < distance) { correction = candidate; distance = std::abs(candidate); }
            }
        };
        for (const int target : {0, span / 2, span}) consider(target);
        if (showExtras_ && showGuides_) for (const auto guide : state.guides)
            if (guide.vertical == vertical) consider(guide.position);
        return delta + correction;
    };
    return {snapAxis(from.x, from.w, offset.x, true, state.width), snapAxis(from.y, from.h, offset.y, false, state.height)};
}

Point App::snappedPoint(Point point) const { return snappedMove({0, 0, 0, 0}, point); }

void App::adjustment(std::optional<AdjustmentKind> kind) {
    auto t = tab();
    if (!t) return;
    auto& doc = t->doc;
    Adjustment values;
    if (kind) {
        if (doc.state().layers.size() >= maxLayers) throw std::runtime_error("This document has reached its layer limit.");
        values.kind = *kind;
        if (*kind == AdjustmentKind::levels) { values.second = 255; values.third = 100; }
    } else {
        if (!doc.active().adjustment || doc.active().locked) return;
        values = *doc.active().adjustment;
    }
    const auto name = adjustmentName(values.kind);
    const auto fields = fieldsFor(values);
    const auto input = prompt(window_, name, fields);
    if (!input) return;
    if (values.kind == AdjustmentKind::levels) {
        values.first = readNumber((*input)[0], 0, 254);
        values.second = readNumber((*input)[1], values.first + 1, 255);
        values.third = readNumber((*input)[2], 10, 1000);
    } else {
        const auto hue = values.kind == AdjustmentKind::hueSaturation;
        values.first = readNumber((*input)[0], hue ? -180 : -100, hue ? 180 : 100);
        values.second = readNumber((*input)[1], -100, 100);
        values.third = hue ? readNumber((*input)[2], -100, 100) : 0;
    }
    doc.begin();
    if (kind) {
        Layer layer;
        layer.name = name;
        layer.adjustment = values;
        layer.mask = maskFromSelection(doc.state().bounds(), doc.selection);
        auto& state = doc.edit();
        state.layers.insert(state.layers.begin() + ++state.active, std::move(layer));
        t->mask = true;
        t->layerScroll = 0;
    } else doc.active().adjustment = values;
    doc.commit(kind ? L"New Adjustment Layer" : L"Edit Adjustment");
    colorTab_ = 2;
    documentChanged();
}

void App::dab(float x, float y, Rect& dirty) {
    auto t = tab();
    auto& doc = t->doc;
    const Brush brush{foreground_, float(brushSize_), hardness_ / 100.0f, flow_ / 100.0f,
        tool_ == Tool::eraser, t->mask};
    const CloneSample source{doc.beforeEdit(), cloneOffset_, cloneMerged_};
    dirty = unite(dirty, stamp(doc.active(), clip(), x, y, brush, tool_ == Tool::clone ? &source : nullptr, selection()));
}
void App::layerStyle(Command command) {
    auto& doc = tab()->doc;
    auto& layer = doc.active();
    if (layer.locked || layer.adjustment) { MessageBeep(MB_ICONWARNING); return; }
    auto style = layer.style;
    if (command == Command::clearStyle) {
        if (style.empty()) return;
        style = {};
    } else {
        const bool stroke = command == Command::strokeStyle, shadow = command == Command::shadowStyle;
        auto& color = stroke ? style.stroke : shadow ? style.shadow : style.overlay;
        const auto title = stroke ? L"Stroke" : shadow ? L"Drop Shadow" : L"Color Overlay";
        const bool enabled = alpha(color) != 0;
        const auto initialColor = enabled ? color : shadow ? rgba(0, 0, 0, 153) :
            stroke ? rgba(0, 0, 0) : rgba(78, 132, 224);
        std::vector<Field> fields{{L"Color (#RRGGBB)", hexColor(initialColor)},
            {L"Opacity (0–100, 0 disables)", std::to_wstring((alpha(initialColor) * 100 + 127) / 255)}};
        if (stroke) fields.push_back({L"Outside size (1–64 px)", std::to_wstring(enabled ? style.strokeWidth : 3)});
        if (shadow) {
            fields.push_back({L"Angle (−180° to 180°)", std::to_wstring(enabled ? style.shadowAngle : 120)});
            fields.push_back({L"Distance (0–512 px)", std::to_wstring(enabled ? style.shadowDistance : 10)});
            fields.push_back({L"Size (0–128 px)", std::to_wstring(enabled ? style.shadowRadius : 12)});
        }
        const auto input = prompt(window_, title, fields);
        if (!input) return;
        const int opacity = readNumber((*input)[1], 0, 100);
        color = opacity ? effectColor((*input)[0], opacity) : 0;
        if (stroke) style.strokeWidth = opacity ? readNumber((*input)[2], 1, 64) : 0;
        if (shadow) {
            style.shadowAngle = opacity ? readNumber((*input)[2], -180, 180) : 0;
            style.shadowDistance = opacity ? readNumber((*input)[3], 0, 512) : 0;
            style.shadowRadius = opacity ? readNumber((*input)[4], 0, 128) : 0;
        }
    }
    doc.begin();
    layer.style = style;
    doc.commit(command == Command::clearStyle ? L"Clear Layer Style" : L"Layer Style");
    documentChanged();
}

void App::beginTransform() {
    auto t = tab();
    if (!t || t->doc.active().locked || t->doc.active().adjustment) { MessageBeep(MB_ICONWARNING); return; }
    const auto& layer = t->doc.active();
    auto area = layer.pixels.contentBounds();
    if (area.empty()) { MessageBeep(MB_ICONWARNING); return; }
    area.x += layer.x; area.y += layer.y;
    t->compare = false;
    beforeBitmaps_.clear();
    t->transform = Transform{area, area};
    repaint();
}

void App::resetTransform() {
    if (auto t = tab(); t && t->transform) {
        t->transform.reset();
        bitmaps_.clear();
        repaint();
    }
}

void App::applyTransform() {
    auto t = tab();
    if (!t || !t->transform) return;
    const auto transform = *t->transform;
    auto& doc = t->doc;
    if (doc.active().locked || doc.active().adjustment) return;
    doc.begin();
    transformLayer(doc.active(), transform.original, transform.box, transform.degrees);
    doc.commit(L"Free Transform");
    t->transform.reset();
    documentChanged();
}

void App::transformNumber(bool angle) {
    auto& transform = *tab()->transform;
    if (angle) {
        int degrees = int(std::round(transform.degrees));
        number(L"Rotation (degrees)", degrees, -360, 360);
        transform.degrees = degrees;
    } else {
        const std::array fields{Field{L"Width (px)", std::to_wstring(transform.box.w)},
            Field{L"Height (px)", std::to_wstring(transform.box.h)}};
        if (auto input = prompt(window_, L"Transform Size", fields)) {
            const auto w = readNumber((*input)[0], 1, maxEdge), h = readNumber((*input)[1], 1, maxEdge);
            transform.box.w = w;
            transform.box.h = h;
        }
    }
    bitmaps_.clear();
    repaint();
}

std::array<Vec, 9> App::transformHandles() const {
    const auto& t = *tab()->transform;
    const double radians = t.degrees * std::numbers::pi / 180;
    const float c = float(std::cos(radians)), s = float(std::sin(radians));
    const auto center = imageToScreen(t.box.x + t.box.w / 2.0f, t.box.y + t.box.h / 2.0f);
    const float sx = t.box.w * tab()->zoom * 0.5f, sy = t.box.h * tab()->zoom * 0.5f;
    std::array<Vec, 9> result;
    for (std::size_t i = 0; i < directions.size(); ++i) {
        const auto d = directions[i];
        result[i] = {center.x + c * sx * d.x - s * sy * d.y, center.y + s * sx * d.x + c * sy * d.y};
    }
    result.back() = {result[1].x + 25 * s, result[1].y - 25 * c};
    return result;
}

bool App::startTransformDrag(Vec p) {
    const auto handles = transformHandles();
    transformHandle_ = -1;
    float nearest = 9;
    for (int i = 0; i < int(handles.size()); ++i) {
        const float distance = std::hypot(handles[i].x - p.x, handles[i].y - p.y);
        if (distance < nearest) { nearest = distance; transformHandle_ = i; }
    }
    auto& transform = *tab()->transform;
    if (transformHandle_ == -1) {
        const auto q = screenToImage(p);
        const auto centerX = transform.box.x + transform.box.w / 2.0;
        const auto centerY = transform.box.y + transform.box.h / 2.0;
        const double angle = transform.degrees * std::numbers::pi / 180;
        const auto dx = q.x - centerX, dy = q.y - centerY;
        if (std::abs(std::cos(angle) * dx + std::sin(angle) * dy) > transform.box.w / 2.0 ||
            std::abs(-std::sin(angle) * dx + std::cos(angle) * dy) > transform.box.h / 2.0) return false;
    }
    transformStart_ = transform;
    drag_ = Drag::transform;
    return true;
}

void App::dragTransform(Vec p) {
    auto& transform = *tab()->transform;
    const auto start = screenToImage(down_), point = screenToImage(p);
    const auto original = transformStart_.box;
    const double dx = point.x - start.x, dy = point.y - start.y;
    const double centerX = original.x + original.w / 2.0, centerY = original.y + original.h / 2.0;
    const double radians = transformStart_.degrees * std::numbers::pi / 180;
    const auto bounded = [](double coordinate) { return int(std::round(std::clamp(coordinate, -65536.0, 65536.0))); };
    if (transformHandle_ == 8) {
        const double before = std::atan2(start.y - centerY, start.x - centerX);
        const double after = std::atan2(point.y - centerY, point.x - centerX);
        transform.degrees = std::remainder(transformStart_.degrees + (after - before) * 180 / std::numbers::pi, 360.0);
        if (GetKeyState(VK_SHIFT) & 0x8000) transform.degrees = std::round(transform.degrees / 15) * 15;
        return;
    }
    if (transformHandle_ < 0) {
        const auto delta = snappedMove(original, {int(std::round(dx)), int(std::round(dy))});
        transform.box.x = bounded(original.x + delta.x);
        transform.box.y = bounded(original.y + delta.y);
        return;
    }
    const auto direction = directions[transformHandle_];
    const double c = std::cos(radians), s = std::sin(radians);
    const double width = original.w + direction.x * (c * dx + s * dy);
    const double height = original.h + direction.y * (-s * dx + c * dy);
    const bool proportional = transform.linked != ((GetKeyState(VK_SHIFT) & 0x8000) != 0);
    int w, h;
    if (proportional) {
        const double xScale = width / original.w, yScale = height / original.h;
        const double scale = std::abs(xScale - 1) > std::abs(yScale - 1) ? xScale : yScale;
        const double largest = double(maxEdge) / std::max(original.w, original.h);
        const double constrained = std::clamp(scale, std::min(1.0 / std::min(original.w, original.h), largest), largest);
        w = std::max(1, int(std::round(original.w * constrained)));
        h = std::max(1, int(std::round(original.h * constrained)));
    } else {
        w = int(std::round(std::clamp(width, 1.0, double(maxEdge))));
        h = int(std::round(std::clamp(height, 1.0, double(maxEdge))));
    }
    const double shiftX = direction.x * (w - original.w) / 2.0, shiftY = direction.y * (h - original.h) / 2.0;
    transform.box = {bounded(centerX + c * shiftX - s * shiftY - w / 2.0),
        bounded(centerY + s * shiftX + c * shiftY - h / 2.0), w, h};
}
} // namespace pf
