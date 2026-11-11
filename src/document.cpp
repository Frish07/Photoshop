#include "document.hpp"

#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <unordered_set>

namespace pf {
namespace {
constexpr int mul255(int a, int b) { return (a * b + 127) / 255; }

template<class F>
void eachTile(Rect area, F operation) {
    if (area.empty()) return;
    for (int ty = tileIndex(area.y); ty <= tileIndex(area.bottom() - 1); ++ty) {
        for (int tx = tileIndex(area.x); tx <= tileIndex(area.right() - 1); ++tx) {
            operation(Coord{tx, ty}, intersect(area, {tx * tileEdge, ty * tileEdge, tileEdge, tileEdge}));
        }
    }
}

Rect local(const Layer& layer, Rect area) {
    return {area.x - layer.x, area.y - layer.y, area.w, area.h};
}

int luminance(Pixel c) {
    return (54 * red(c) + 183 * green(c) + 19 * blue(c) + 128) >> 8;
}

int blendChannel(int s, int d, Blend mode) {
    auto burn = [d](int source) { return d == 255 ? 255 : source == 0 ? 0 : 255 - std::min(255, (255 - d) * 255 / source); };
    auto dodge = [d](int source) { return d == 0 ? 0 : source == 255 ? 255 : std::min(255, d * 255 / (255 - source)); };
    switch (mode) {
    case Blend::multiply: return mul255(s, d);
    case Blend::screen: return 255 - mul255(255 - s, 255 - d);
    case Blend::overlay: return d < 128 ? mul255(2 * d, s) : 255 - mul255(2 * (255 - d), 255 - s);
    case Blend::darken: return std::min(s, d);
    case Blend::lighten: return std::max(s, d);
    case Blend::difference: return std::abs(s - d);
    case Blend::colorBurn: return burn(s);
    case Blend::linearBurn: return std::max(0, s + d - 255);
    case Blend::colorDodge: return dodge(s);
    case Blend::linearDodge: return std::min(255, s + d);
    case Blend::softLight: {
        const auto b = d / 255.0, a = s / 255.0;
        const auto f = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
        return int(255 * (a <= 0.5 ? b - (1 - 2 * a) * b * (1 - b) : b + (2 * a - 1) * (f - b)) + 0.5);
    }
    case Blend::hardLight: return s < 128 ? mul255(2 * s, d) : 255 - mul255(2 * (255 - s), 255 - d);
    case Blend::vividLight: return s < 128 ? burn(2 * s) : dodge(2 * s - 255);
    case Blend::linearLight: return std::clamp(d + 2 * s - 255, 0, 255);
    case Blend::pinLight: return s < 128 ? std::min(d, 2 * s) : std::max(d, 2 * s - 255);
    case Blend::hardMix: return s + d >= 255 ? 255 : 0;
    case Blend::exclusion: return s + d - mul255(2 * s, d);
    case Blend::subtract: return std::max(0, d - s);
    case Blend::divide: return s ? std::min(255, d * 255 / s) : 255;
    default: return s;
    }
}

Pixel blendColor(Pixel source, Pixel backdrop, Blend mode) {
    if (mode < Blend::darkerColor) return rgba(blendChannel(red(source), red(backdrop), mode),
        blendChannel(green(source), green(backdrop), mode), blendChannel(blue(source), blue(backdrop), mode));
    using RGB = std::array<double, 3>;
    auto decode = [](Pixel p) -> RGB { return {double(red(p)), double(green(p)), double(blue(p))}; };
    const auto s = decode(source), b = decode(backdrop);
    if (mode == Blend::darkerColor || mode == Blend::lighterColor) {
        const double a = s[0] + s[1] + s[2], d = b[0] + b[1] + b[2];
        return (mode == Blend::darkerColor ? a < d : a > d) ? source : backdrop;
    }
    auto lum = [](RGB v) { return 0.3 * v[0] + 0.59 * v[1] + 0.11 * v[2]; };
    auto sat = [](RGB v) { const auto [lo, hi] = std::minmax_element(v.begin(), v.end()); return *hi - *lo; };
    auto setLum = [&](RGB v, double l) {
        const auto delta = l - lum(v);
        for (auto& x : v) x += delta;
        const auto [minIt, maxIt] = std::minmax_element(v.begin(), v.end());
        const auto low = *minIt, high = *maxIt;
        if (low < 0 && l > low) for (auto& x : v) x = l + (x - l) * l / (l - low);
        if (high > 255 && high > l) for (auto& x : v) x = l + (x - l) * (255 - l) / (high - l);
        return v;
    };
    auto setSat = [](RGB v, double saturation) {
        std::array<int, 3> order{0, 1, 2};
        std::sort(order.begin(), order.end(), [&](int a, int b) { return v[a] < v[b]; });
        const auto [lo, mid, hi] = order;
        if (v[hi] > v[lo]) {
            v[mid] = (v[mid] - v[lo]) * saturation / (v[hi] - v[lo]);
            v[hi] = saturation;
        } else v[mid] = v[hi] = 0;
        v[lo] = 0;
        return v;
    };
    const auto result = mode == Blend::hue ? setLum(setSat(s, sat(b)), lum(b)) :
        mode == Blend::saturation ? setLum(setSat(b, sat(s)), lum(b)) :
        mode == Blend::color ? setLum(s, lum(b)) : setLum(b, lum(s));
    auto byte = [](double v) { return std::clamp(int(v + 0.5), 0, 255); };
    return rgba(byte(result[0]), byte(result[1]), byte(result[2]));
}

class AdjustmentMap {
public:
    explicit AdjustmentMap(const Adjustment& value) : value_(value) {
        if (value.kind == AdjustmentKind::hueSaturation) return;
        for (int i = 0; i < 256; ++i) {
            if (value.kind == AdjustmentKind::levels) {
                const double input = std::clamp(double(i - value.first) / (value.second - value.first), 0.0, 1.0);
                lookup_[i] = int(255 * std::pow(input, 100.0 / value.third) + 0.5);
            } else {
                const int contrast = (i - 128) * (100 + value.second) / 100 + 128;
                lookup_[i] = std::clamp(contrast + value.first * 255 / 100, 0, 255);
            }
        }
    }

    Pixel apply(Pixel current, int opacity, Blend mode) const {
        const int a = alpha(current);
        if (!a || !opacity) return current;
        const Pixel source = straight(current);
        const auto adjusted = value_.kind == AdjustmentKind::hueSaturation ? hue(source) :
            rgba(lookup_[red(source)], lookup_[green(source)], lookup_[blue(source)], a);
        const auto blended = mode == Blend::normal ? adjusted : blendColor(adjusted, source, mode);
        auto mix = [&](int before, int after) { return (before * (255 - opacity) + after * opacity + 127) / 255; };
        return premultiply(rgba(mix(red(source), red(blended)), mix(green(source), green(blended)),
            mix(blue(source), blue(blended)), a));
    }

private:
    Pixel hue(Pixel source) const {
        const double r = red(source) / 255.0, g = green(source) / 255.0, b = blue(source) / 255.0;
        const double high = std::max({r, g, b}), low = std::min({r, g, b});
        const double chroma = high - low, light = (high + low) * 0.5;
        double h = 0, s = 0;
        if (chroma > 0) {
            h = (high == r ? (g - b) / chroma : high == g ? 2 + (b - r) / chroma : 4 + (r - g) / chroma) / 6;
            s = chroma / (1 - std::abs(2 * light - 1));
        }
        h += value_.first / 360.0;
        h -= std::floor(h);
        s = std::clamp(s * (1 + value_.second / 100.0), 0.0, 1.0);
        const double l = std::clamp(light + value_.third / 100.0, 0.0, 1.0);
        const double amplitude = s * std::min(l, 1 - l);
        auto channel = [&](int offset) {
            const double k = std::fmod(offset + h * 12, 12.0);
            return std::clamp(int(255 * (l - amplitude * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}))) + 0.5), 0, 255);
        };
        return rgba(channel(0), channel(8), channel(4), alpha(source));
    }

    Adjustment value_;
    std::array<int, 256> lookup_{};
};

Pixel paint(Pixel dst, Pixel src, int opacity, bool erase, bool mask) {
    const int a = mul255(alpha(src), opacity);
    if (a == 0) return dst;
    if (mask) {
        const int v = mul255(red(dst), 255 - a) + mul255(erase ? 255 : luminance(src), a);
        return rgba(v, v, v);
    }
    if (erase) {
        const int remaining = mul255(alpha(dst), 255 - a);
        return remaining ? (dst & 0xffffff) | (Pixel(remaining) << 24) : 0;
    }
    return straight(over(premultiply(dst), src, opacity));
}

template<class F>
void modify(Plane& plane, Rect area, bool existingOnly, F operation, const Selection* selection = nullptr, Point origin = {}) {
    if (selection && selection->rectangular()) {
        auto selected = selection->bounds();
        selected.x -= origin.x; selected.y -= origin.y;
        area = intersect(area, selected);
        selection = nullptr;
    }
    eachTile(area, [&](Coord coord, Rect clip) {
        const Tile* old = plane.tile(coord);
        if (existingOnly && !old) return;
        Tile* changed = nullptr;
        auto span = [&](int y, int first, int last) {
            for (int x = first; x < last; ++x) {
                const int i = (y - coord.y * tileEdge) * tileEdge + x - coord.x * tileEdge;
                const Pixel before = changed ? changed->px[i] : old ? old->px[i] : plane.background();
                const Pixel after = operation(before, x, y);
                if (before == after) continue;
                if (!changed) changed = &plane.edit(coord);
                changed->px[i] = after;
            }
        };
        for (int y = clip.y; y < clip.bottom(); ++y) {
            if (!selection) span(y, clip.x, clip.right());
            else {
                Run rectangular;
                const auto runs = selection->row(y + origin.y, rectangular);
                auto run = std::lower_bound(runs.begin(), runs.end(), clip.x + origin.x,
                    [](Run a, int x) { return a.last <= x; });
                for (; run != runs.end() && run->first < clip.right() + origin.x; ++run)
                    span(y, std::max(clip.x, run->first - origin.x), std::min(clip.right(), run->last - origin.x));
            }
        }
    });
}

std::size_t estimate(const State& a, const State& b) {
    // Include metadata, distinct tile maps and tiles retained by either endpoint.
    std::unordered_set<const void*> mapsA, mapsB;
    std::unordered_set<const Tile*> tilesA, tilesB;
    std::unordered_map<const void*, const Plane::Tiles*> planesA, planesB;
    auto collect = [](const State& state, auto& maps, auto& planes) {
        for (const auto& l : state.layers) {
            maps.insert(l.pixels.identity());
            planes.emplace(l.pixels.identity(), &l.pixels.tiles());
            if (l.mask) {
                maps.insert(l.mask->identity());
                planes.emplace(l.mask->identity(), &l.mask->tiles());
            }
        }
    };
    collect(a, mapsA, planesA);
    collect(b, mapsB, planesB);
    std::size_t bytes = sizeof(State) + sizeof(Layer) * (a.layers.size() + b.layers.size());
    auto differing = [&](const auto& planes, const auto& otherMaps, auto& tiles) {
        for (const auto& [id, map] : planes) {
            if (otherMaps.contains(id)) continue;
            bytes += sizeof(Plane::Tiles) + map->size() * 64;
            for (const auto& [coord, tile] : *map) tiles.insert(tile.get());
        }
    };
    differing(planesA, mapsB, tilesA);
    differing(planesB, mapsA, tilesB);
    // Shared maps may contain tiles also used by a changed layer (for example after duplication).
    auto unchanged = [&](const auto& planes, const auto& otherMaps) {
        for (const auto& [id, map] : planes) {
            if (!otherMaps.contains(id)) continue;
            for (const auto& [coord, tile] : *map) {
                tilesA.erase(tile.get());
                tilesB.erase(tile.get());
            }
        }
    };
    if (!tilesA.empty() || !tilesB.empty()) unchanged(planesA, mapsB);
    for (auto tile : tilesA) if (!tilesB.contains(tile)) bytes += sizeof(Tile) + 32;
    for (auto tile : tilesB) if (!tilesA.contains(tile)) bytes += sizeof(Tile) + 32;
    return bytes;
}

bool same(const State& a, const State& b) {
    if (a.width != b.width || a.height != b.height || a.ppi != b.ppi || a.guides != b.guides ||
        a.layers.size() != b.layers.size()) return false;
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        const auto& x = a.layers[i];
        const auto& y = b.layers[i];
        if (x.name != y.name || x.x != y.x || x.y != y.y || x.opacity != y.opacity || x.blend != y.blend ||
            x.visible != y.visible || x.locked != y.locked || x.pixels.identity() != y.pixels.identity() ||
            x.adjustment != y.adjustment || x.style != y.style || x.fillOpacity != y.fillOpacity ||
            x.maskEnabled != y.maskEnabled ||
            x.mask.has_value() != y.mask.has_value() ||
            (x.mask && x.mask->identity() != y.mask->identity())) return false;
    }
    return true;
}

Plane uniformPlane(int width, int height, Pixel color) {
    Plane plane;
    if (!alpha(color)) return plane;
    std::array<std::shared_ptr<Tile>, 4> patterns;
    eachTile({0, 0, width, height}, [&](Coord coord, Rect area) {
        const int pattern = (area.w != tileEdge) + 2 * (area.h != tileEdge);
        auto& tile = patterns[pattern];
        if (!tile) {
            tile = std::make_shared<Tile>();
            for (int y = 0; y < area.h; ++y) std::fill_n(tile->px.begin() + y * tileEdge, area.w, color);
        }
        plane.assign(coord, tile);
    });
    return plane;
}

std::optional<Pixel> uniformColor(const Plane& plane, int width, int height) {
    if (plane.tiles().size() != std::size_t((width + tileEdge - 1) / tileEdge) * ((height + tileEdge - 1) / tileEdge))
        return std::nullopt;
    const auto color = plane.pixel(0, 0);
    std::array<std::unordered_set<const Tile*>, 4> checked;
    bool solid = true;
    eachTile({0, 0, width, height}, [&](Coord coord, Rect area) {
        if (!solid) return;
        const auto tile = plane.tile(coord);
        if (!tile) { solid = false; return; }
        const int pattern = (area.w != tileEdge) + 2 * (area.h != tileEdge);
        if (!checked[pattern].insert(tile).second) return;
        for (int y = 0; y < tileEdge && solid; ++y) for (int x = 0; x < tileEdge; ++x) {
            if (tile->px[y * tileEdge + x] != (x < area.w && y < area.h ? color : 0)) { solid = false; break; }
        }
    });
    return solid ? std::optional(color) : std::nullopt;
}

Pixel sampleLinear(const Plane& input, double x, double y) {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    const Pixel a = premultiply(input.pixel(ix, iy)), b = premultiply(input.pixel(ix + 1, iy));
    const Pixel c = premultiply(input.pixel(ix, iy + 1)), d = premultiply(input.pixel(ix + 1, iy + 1));
    auto mix = [&](auto get) {
        return int((1 - fy) * ((1 - fx) * get(a) + fx * get(b)) + fy * ((1 - fx) * get(c) + fx * get(d)) + 0.5);
    };
    return straight(rgba(mix(red), mix(green), mix(blue), mix(alpha)));
}

class InverseTransform {
public:
    explicit InverseTransform(TransformView view)
        : view_(view), c_(std::cos(view.degrees * std::numbers::pi / 180)), s_(std::sin(view.degrees * std::numbers::pi / 180)),
          centerX_(view.destination.x + view.destination.w / 2.0), centerY_(view.destination.y + view.destination.h / 2.0),
          scaleX_(double(view.original.w) / view.destination.w), scaleY_(double(view.original.h) / view.destination.h) {}

    Rect extent() const {
        const auto& dest = view_.destination;
        const double halfW = (std::abs(c_) * dest.w + std::abs(s_) * dest.h) / 2;
        const double halfH = (std::abs(s_) * dest.w + std::abs(c_) * dest.h) / 2;
        const int left = int(std::floor(centerX_ - halfW + 1e-9)), top = int(std::floor(centerY_ - halfH + 1e-9));
        return {left, top, int(std::ceil(centerX_ + halfW - 1e-9)) - left, int(std::ceil(centerY_ + halfH - 1e-9)) - top};
    }

    bool map(int x, int y, double& sourceX, double& sourceY) const {
        const double dx = x + 0.5 - centerX_, dy = y + 0.5 - centerY_;
        const double u = c_ * dx + s_ * dy + view_.destination.w / 2.0;
        const double v = -s_ * dx + c_ * dy + view_.destination.h / 2.0;
        if (u < 0 || u >= view_.destination.w || v < 0 || v >= view_.destination.h) return false;
        sourceX = view_.original.x + std::clamp(u * scaleX_ - 0.5, 0.0, view_.original.w - 1.0);
        sourceY = view_.original.y + std::clamp(v * scaleY_ - 0.5, 0.0, view_.original.h - 1.0);
        return true;
    }

private:
    TransformView view_;
    double c_, s_, centerX_, centerY_, scaleX_, scaleY_;
};

Pixel styledPixel(Pixel pixel, const Layer& layer) {
    const int tint = alpha(layer.style.overlay);
    if (!tint && layer.fillOpacity == 255) return pixel;
    if (!alpha(pixel)) return 0;
    const int base = mul255(layer.fillOpacity, 255 - tint), total = base + tint;
    if (!total) return 0;
    auto mix = [&](int source, int overlay) { return (source * base + overlay * tint + total / 2) / total; };
    return rgba(mix(red(pixel), red(layer.style.overlay)), mix(green(pixel), green(layer.style.overlay)),
        mix(blue(pixel), blue(layer.style.overlay)), mul255(alpha(pixel), total));
}

void layerEffects(const Layer& layer, Rect area, int step, int width, int height, std::span<Pixel> pixels, int stride,
    const InverseTransform* transform = nullptr) {
    if (!alpha(layer.style.shadow) && !alpha(layer.style.stroke)) return;
    Coord previous{std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
    const Tile* tile = nullptr;
    const Tile* mask = nullptr;
    const Plane* maskPlane = layer.mask && layer.maskEnabled ? &*layer.mask : nullptr;
    const int maskDefault = maskPlane ? red(maskPlane->background()) : 255;
    auto sourceAlpha = [&](int x, int y) {
        if (transform) {
            double px{}, py{};
            if (!transform->map(x, y, px, py)) return 0;
            x = int(std::round(px)); y = int(std::round(py));
        }
        x -= layer.x; y -= layer.y;
        const Coord coord{tileIndex(x), tileIndex(y)};
        if (coord != previous) {
            previous = coord;
            tile = layer.pixels.tile(coord);
            mask = maskPlane ? maskPlane->tile(coord) : nullptr;
        }
        if (!tile) return 0;
        const int i = (y - coord.y * tileEdge) * tileEdge + x - coord.x * tileEdge;
        return mul255(alpha(tile->px[i]), mask ? red(mask->px[i]) : maskDefault);
    };
    auto render = [&](Pixel color, int radius, Point offset, bool stroke) {
        const int h = height + 2 * radius;
        const int length = std::max(width, height) + 2 * radius;
        std::vector<std::uint8_t> horizontal(std::size_t(width) * h), input(length), temporary(length);
        std::vector<int> queue(stroke ? length : 0);
        auto reduce = [&](std::span<const std::uint8_t> row, int r, auto emit) {
            const int window = 2 * r + 1, n = int(row.size());
            if (stroke) {
                int first = 0, last = 0;
                for (int i = 0; i < n; ++i) {
                    while (first < last && row[queue[last - 1]] <= row[i]) --last;
                    queue[last++] = i;
                    if (queue[first] <= i - window) ++first;
                    if (i >= window - 1) emit(i - window + 1, row[queue[first]]);
                }
            } else {
                int sum = 0;
                for (int i = 0; i < window; ++i) sum += row[i];
                for (int i = 0; i <= n - window; ++i) {
                    emit(i, (sum + window / 2) / window);
                    if (i < n - window) sum += row[i + window] - row[i];
                }
            }
        };
        auto filter = [&](int count, auto emit) {
            auto row = std::span(input).first(std::size_t(count) + 2 * radius);
            if (stroke || radius <= 1) reduce(row, radius, emit);
            else {
                const int inner = radius / 2;
                reduce(row, inner, [&](int i, int a) { temporary[i] = std::uint8_t(a); });
                reduce(std::span(temporary).first(row.size() - 2 * inner), radius - inner, emit);
            }
        };
        const int originX = area.x + step / 2 - offset.x;
        const int originY = area.y + step / 2 - offset.y;
        for (int y = 0; y < h; ++y) {
            const int gy = originY + (y - radius) * step;
            for (int x = 0; x < width + 2 * radius; ++x)
                input[x] = std::uint8_t(sourceAlpha(originX + (x - radius) * step, gy));
            filter(width, [&](int x, int a) { horizontal[std::size_t(y) * width + x] = std::uint8_t(a); });
        }
        for (int x = 0; x < width; ++x) {
            for (int y = 0; y < h; ++y) input[y] = horizontal[std::size_t(y) * width + x];
            filter(height, [&](int y, int a) {
                const int coverage = stroke ? std::max(0, a - sourceAlpha(originX + x * step, originY + y * step)) : a;
                auto& out = pixels[std::size_t(y) * stride + x];
                out = over(out, color, mul255(layer.opacity, coverage), stroke ? Blend::normal : Blend::multiply);
            });
        }
    };
    if (alpha(layer.style.shadow)) {
        const auto radians = layer.style.shadowAngle * std::numbers::pi / 180;
        const Point offset{int(std::round(-std::cos(radians) * layer.style.shadowDistance)),
            int(std::round(std::sin(radians) * layer.style.shadowDistance))};
        render(layer.style.shadow, (layer.style.shadowRadius + step / 2) / step, offset, false);
    }
    if (alpha(layer.style.stroke)) render(layer.style.stroke, (layer.style.strokeWidth + step - 1) / step, {}, true);
}
} // namespace

Rect intersect(Rect a, Rect b) {
    const int x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0, std::min(a.right(), b.right()) - x),
        std::max(0, std::min(a.bottom(), b.bottom()) - y)};
}
Rect unite(Rect a, Rect b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.right(), b.right()) - x, std::max(a.bottom(), b.bottom()) - y};
}
Rect between(Point a, Point b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x) + 1, std::abs(a.y - b.y) + 1};
}
int tileIndex(int x) { return x >= 0 ? x / tileEdge : -1 - ((-1 - x) / tileEdge); }

bool validAdjustment(const Adjustment& value) {
    if (value.kind == AdjustmentKind::levels)
        return value.first >= 0 && value.first < value.second && value.second <= 255 && value.third >= 10 && value.third <= 1000;
    if (value.kind == AdjustmentKind::tone)
        return value.first >= -100 && value.first <= 100 && value.second >= -100 && value.second <= 100 && value.third == 0;
    return value.kind == AdjustmentKind::hueSaturation && value.first >= -180 && value.first <= 180 &&
        value.second >= -100 && value.second <= 100 && value.third >= -100 && value.third <= 100;
}
bool validLayerStyle(const LayerStyle& style) {
    return style.strokeWidth >= (alpha(style.stroke) ? 1 : 0) && style.strokeWidth <= 64 &&
        style.shadowRadius >= 0 && style.shadowRadius <= 128 && style.shadowDistance >= 0 && style.shadowDistance <= 512 &&
        style.shadowAngle >= -180 && style.shadowAngle <= 180;
}

Pixel premultiply(Pixel c) {
    const int a = alpha(c);
    if (a == 255) return c;
    return rgba(mul255(red(c), a), mul255(green(c), a), mul255(blue(c), a), a);
}
Pixel straight(Pixel c) {
    const int a = alpha(c);
    if (a == 0 || a == 255) return c;
    auto channel = [a](int v) { return std::min(255, (v * 255 + a / 2) / a); };
    return rgba(channel(red(c)), channel(green(c)), channel(blue(c)), a);
}
Pixel over(Pixel d, Pixel s, int opacity, Blend mode) {
    const int sa = mul255(alpha(s), opacity);
    if (sa == 0) return d;
    if (sa == 255 && mode == Blend::normal) return s;
    const int da = alpha(d);
    const int a = sa + mul255(da, 255 - sa);
    const bool blend = mode != Blend::normal && da != 0;
    const Pixel result = blend ? blendColor(s, straight(d), mode) : s;
    auto channel = [&](int sc, int dc, int bc) {
        const int mixed = blend ? mul255(sc, 255 - da) + mul255(bc, da) : sc;
        return std::min(a, mul255(mixed, sa) + mul255(dc, 255 - sa));
    };
    return rgba(channel(red(s), red(d), red(result)), channel(green(s), green(d), green(result)),
        channel(blue(s), blue(d), blue(result)), a);
}

std::size_t CoordHash::operator()(Coord c) const {
    std::uint64_t value = (std::uint64_t(std::uint32_t(c.x)) << 32) | std::uint32_t(c.y);
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ull;
    return std::size_t(value ^ (value >> 27));
}
const Tile* Plane::tile(Coord c) const {
    auto i = tiles_->find(c);
    return i == tiles_->end() ? nullptr : i->second.get();
}
Pixel Plane::pixel(int x, int y) const {
    const Coord c{tileIndex(x), tileIndex(y)};
    const auto* t = tile(c);
    return t ? t->px[(y - c.y * tileEdge) * tileEdge + x - c.x * tileEdge] : background_;
}
void Plane::detach() {
    if (tiles_.use_count() != 1) tiles_ = std::make_shared<Tiles>(*tiles_);
}
Tile& Plane::edit(Coord c) {
    detach();
    auto& t = (*tiles_)[c];
    if (!t) {
        t = std::make_shared<Tile>();
        if (background_) t->px.fill(background_);
    } else if (t.use_count() != 1) {
        t = std::make_shared<Tile>(*t);
    }
    return *t;
}
void Plane::assign(Coord c, std::shared_ptr<Tile> tile) {
    detach();
    tiles_->emplace(c, std::move(tile));
}
void Plane::prune() {
    // Shared tiles were untouched by this edit. Newly written tiles can become fully empty.
    if (tiles_.use_count() != 1) return;
    std::erase_if(*tiles_, [&](const auto& pair) {
        const auto& t = pair.second;
        return t.use_count() == 1 && std::all_of(t->px.begin(), t->px.end(), [&](Pixel p) { return p == background_; });
    });
}
Rect Plane::extent() const {
    Rect bounds;
    for (const auto& [c, tile] : *tiles_) bounds = unite(bounds, {c.x * tileEdge, c.y * tileEdge, tileEdge, tileEdge});
    return bounds;
}
Rect Plane::contentBounds() const {
    Rect bounds;
    std::unordered_map<const Tile*, Rect> checked;
    for (const auto& [coord, tile] : *tiles_) {
        const auto [entry, inserted] = checked.try_emplace(tile.get());
        auto& area = entry->second;
        if (inserted) {
            for (int y = 0; y < tileEdge; ++y) for (int x = 0; x < tileEdge; ++x) {
                if (alpha(tile->px[y * tileEdge + x])) area = unite(area, {x, y, 1, 1});
            }
        }
        bounds = unite(bounds, {area.x + coord.x * tileEdge, area.y + coord.y * tileEdge, area.w, area.h});
    }
    return bounds;
}

State blankDocument(int width, int height, double ppi, Pixel background) {
    State state{width, height, {Layer{}}, 0, ppi};
    state.layers[0].name = L"Layer 1";
    if (alpha(background)) {
        Layer layer;
        layer.name = L"Background";
        layer.locked = true;
        layer.pixels = uniformPlane(width, height, background);
        state.layers.insert(state.layers.begin(), std::move(layer));
        state.active = 1;
    }
    return state;
}
Plane maskFromSelection(Rect canvas, const std::optional<Selection>& selection, Point origin) {
    constexpr Pixel white = 0xffffffff, black = 0xff000000;
    Plane mask(white);
    if (!selection) return mask;
    auto dark = std::make_shared<Tile>();
    dark->px.fill(black);
    eachTile(canvas, [&](Coord coord, Rect area) {
        const auto global = Rect{area.x + origin.x, area.y + origin.y, area.w, area.h};
        const auto reveal = intersect(global, selection->bounds());
        if (selection->rectangular() && reveal == global) return;
        if (reveal.empty() && area.w == tileEdge && area.h == tileEdge) {
            mask.assign(coord, dark);
            return;
        }
        Tile tile;
        tile.px.fill(white);
        int revealed = 0;
        for (int y = area.y; y < area.bottom(); ++y) {
            auto* row = tile.px.data() + (y - coord.y * tileEdge) * tileEdge + area.x - coord.x * tileEdge;
            std::fill_n(row, area.w, black);
            Run scratch;
            for (const auto span : selection->row(y + origin.y, scratch)) {
                const int first = std::max(global.x, span.first), last = std::min(global.right(), span.last);
                if (first < last) {
                    std::fill(row + first - global.x, row + last - global.x, white);
                    revealed += last - first;
                }
            }
        }
        if (revealed == area.w * area.h) return;
        if (!revealed && area.w == tileEdge && area.h == tileEdge) mask.assign(coord, dark);
        else mask.assign(coord, std::make_shared<Tile>(std::move(tile)));
    });
    return mask;
}

Document::Document(State state, std::wstring label, std::size_t limit)
    : state_(std::move(state)), historyLimit_(limit) {
    if (state_.layers.empty()) state_.layers.emplace_back();
    state_.active = std::clamp(state_.active, 0, int(state_.layers.size()) - 1);
    history_.push_back({state_, {}, std::move(label), 0, 0, 0});
}
void Document::select(int index) {
    if (index >= 0 && index < int(state_.layers.size())) state_.active = index;
}
void Document::begin() {
    if (!baseline_) {
        baseline_ = state_;
        baselineSelection_ = selection;
    }
}
bool Document::commit(std::wstring label) {
    if (!baseline_) return false;
    for (auto& layer : state_.layers) {
        layer.pixels.prune();
        if (layer.mask) layer.mask->prune();
    }
    const bool changed = !same(*baseline_, state_);
    const bool selectionChanged = selection != baselineSelection_;
    if (!changed && !selectionChanged) {
        baseline_.reset();
        baselineSelection_.reset();
        return false;
    }
    std::size_t cost = changed ? estimate(*baseline_, state_) : sizeof(Entry) + state_.layers.size() * sizeof(Layer);
    if (selectionChanged) cost += (selection ? selection->memoryBytes() : 0) + (baselineSelection_ ? baselineSelection_->memoryBytes() : 0);
    Entry entry{state_, selection, std::move(label), nextId_, changed ? nextId_ : contentId(), cost};
    history_.reserve(index_ + 2);
    while (history_.size() > index_ + 1) {
        historyBytes_ -= history_.back().cost;
        history_.pop_back();
    }
    history_.push_back(std::move(entry));
    ++nextId_;
    baseline_.reset();
    baselineSelection_.reset();
    historyBytes_ += cost;
    while (history_.size() > 1 && (historyBytes_ > historyLimit_ || history_.size() > 80)) {
        historyBytes_ -= history_[1].cost;
        history_.erase(history_.begin());
        history_.front().cost = 0;
    }
    index_ = history_.size() - 1;
    return true;
}
void Document::cancel() {
    if (baseline_) {
        state_ = std::move(*baseline_);
        selection = std::move(baselineSelection_);
    }
    baseline_.reset();
    baselineSelection_.reset();
}
bool Document::jump(std::size_t position) {
    if (baseline_ || position >= history_.size() || position == index_) return false;
    state_ = State(history_[position].state);
    selection = history_[position].selection;
    index_ = position;
    return true;
}
bool Document::undo() { return index_ > 0 && jump(index_ - 1); }
bool Document::redo() { return jump(index_ + 1); }
std::size_t Document::pixelBytes() const {
    std::unordered_set<const Tile*> used;
    for (const auto& layer : state_.layers) {
        for (const auto& [key, tile] : layer.pixels.tiles()) used.insert(tile.get());
        if (layer.mask) for (const auto& [key, tile] : layer.mask->tiles()) used.insert(tile.get());
    }
    return used.size() * sizeof(Tile);
}

Rect stamp(Layer& layer, Rect clip, float x, float y, const Brush& brush, const CloneSample* clone, const Selection* selection) {
    const float radius = brush.diameter * 0.5f;
    Rect region{int(std::floor(x - radius)), int(std::floor(y - radius)),
        int(std::ceil(x + radius) - std::floor(x - radius)), int(std::ceil(y + radius) - std::floor(y - radius))};
    region = intersect(region, clip);
    if (clone) {
        auto source = clone->source.bounds();
        source.x -= clone->offset.x; source.y -= clone->offset.y;
        region = intersect(region, source);
    }
    if (region.empty()) return {};
    std::vector<Pixel> merged;
    if (clone && clone->merged) {
        merged.resize(std::size_t(region.w) * region.h);
        composite(clone->source, {region.x + clone->offset.x, region.y + clone->offset.y, region.w, region.h}, merged, region.w);
    }
    const float inner = std::max(0.0f, radius * brush.hardness - 0.5f);
    const float outer = radius + 0.35f;
    const int opacity = int(brush.opacity * 255 + 0.5f);
    auto& plane = brush.mask ? *layer.mask : layer.pixels;
    modify(plane, local(layer, region), brush.erase && !brush.mask, [&](Pixel old, int lx, int ly) {
        const float dx = lx + layer.x + 0.5f - x, dy = ly + layer.y + 0.5f - y;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= outer * outer) return old;
        const float coverage = d2 <= inner * inner ? 1.0f : (outer - std::sqrt(d2)) / (outer - inner);
        Pixel color = brush.color;
        if (clone) {
            const int gx = lx + layer.x, gy = ly + layer.y;
            const auto& source = clone->source.layers[clone->source.active];
            color = clone->merged ? straight(merged[std::size_t(gy - region.y) * region.w + gx - region.x]) :
                source.pixels.pixel(gx + clone->offset.x - source.x, gy + clone->offset.y - source.y);
        }
        return paint(old, color, int(coverage * opacity + 0.5f), brush.erase, brush.mask);
    }, selection, {layer.x, layer.y});
    return region;
}
void fill(Layer& layer, Rect area, Pixel color, bool erase, bool mask, const Selection* selection) {
    auto& plane = mask ? *layer.mask : layer.pixels;
    modify(plane, local(layer, area), erase && !mask, [&](Pixel old, int, int) {
        return paint(old, color, 255, erase, mask);
    }, selection, {layer.x, layer.y});
}
void gradient(Layer& layer, Rect area, Point from, Point to, Pixel start, Pixel end, bool mask, const Selection* selection) {
    const double dx = to.x - from.x, dy = to.y - from.y;
    const double length = std::max(1.0, dx * dx + dy * dy);
    auto& plane = mask ? *layer.mask : layer.pixels;
    modify(plane, local(layer, area), false, [&](Pixel old, int x, int y) {
        const double t = std::clamp(((x + layer.x - from.x) * dx + (y + layer.y - from.y) * dy) / length, 0.0, 1.0);
        auto mix = [t](int a, int b) { return int(a + (b - a) * t + 0.5); };
        return paint(old, rgba(mix(red(start), red(end)), mix(green(start), green(end)),
            mix(blue(start), blue(end)), mix(alpha(start), alpha(end))), 255, false, mask);
    }, selection, {layer.x, layer.y});
}
void ellipse(Layer& layer, Rect area, Rect clip, Pixel color, const Selection* selection) {
    const double rx = area.w / 2.0, ry = area.h / 2.0;
    modify(layer.pixels, local(layer, intersect(area, clip)), false, [&](Pixel old, int x, int y) {
        const double dx = (x + layer.x + 0.5 - area.x - rx) / rx;
        const double dy = (y + layer.y + 0.5 - area.y - ry) / ry;
        const double edge = (1.0 - std::sqrt(dx * dx + dy * dy)) * std::min(rx, ry) + 0.5;
        return paint(old, color, int(std::clamp(edge, 0.0, 1.0) * 255), false, false);
    }, selection, {layer.x, layer.y});
}
Rect flood(Layer& layer, Rect clip, Point point, Pixel color, int tolerance, bool mask, const Selection* selection) {
    if (!clip.contains(point) || (selection && !selection->contains(point))) return {};
    auto& plane = mask ? *layer.mask : layer.pixels;
    clip = local(layer, clip);
    point = {point.x - layer.x, point.y - layer.y};
    const Pixel target = plane.pixel(point.x, point.y);
    if (!tolerance && paint(target, color, 255, false, mask) == target) return {};
    const std::size_t count = std::size_t(clip.w) * clip.h;
    std::vector<std::uint64_t> visited((count + 63) / 64);
    std::vector<Point> pending{point};
    auto seen = [&](int x, int y, bool mark) {
        const auto i = std::size_t(y - clip.y) * clip.w + (x - clip.x);
        auto& word = visited[i >> 6];
        const auto bit = 1ull << (i & 63);
        const bool wasSet = (word & bit) != 0;
        if (mark) word |= bit;
        return wasSet;
    };
    auto matches = [&](int x, int y) {
        if (seen(x, y, false) || (selection && !selection->contains({x + layer.x, y + layer.y}))) return false;
        const auto c = plane.pixel(x, y);
        return std::max({std::abs(red(c) - red(target)), std::abs(green(c) - green(target)),
            std::abs(blue(c) - blue(target)), std::abs(alpha(c) - alpha(target))}) <= tolerance;
    };
    Rect changed;
    while (!pending.empty()) {
        const auto [x, y] = pending.back();
        pending.pop_back();
        if (!matches(x, y)) continue;
        int left = x, right = x + 1;
        while (left > clip.x && matches(left - 1, y)) --left;
        while (right < clip.right() && matches(right, y)) ++right;
        for (int xx = left; xx < right; ++xx) seen(xx, y, true);
        modify(plane, {left, y, right - left, 1}, false,
            [&](Pixel p, int, int) { return paint(p, color, 255, false, mask); });
        changed = unite(changed, {left + layer.x, y + layer.y, right - left, 1});
        for (int yy : {y - 1, y + 1}) {
            if (yy < clip.y || yy >= clip.bottom()) continue;
            bool inside = false;
            for (int xx = left; xx < right; ++xx) {
                const bool match = matches(xx, yy);
                if (match && !inside) pending.push_back({xx, yy});
                inside = match;
            }
        }
    }
    return changed;
}
void adjust(Layer& layer, Rect area, Filter filter, int amount, const Selection* selection) {
    modify(layer.pixels, local(layer, area), true, [&](Pixel c, int, int) {
        if (!alpha(c)) return c;
        auto channel = [&](int v) {
            if (filter == Filter::invert) return 255 - v;
            if (filter == Filter::grayscale) return luminance(c);
            if (filter == Filter::brightness) return std::clamp(v + amount, 0, 255);
            return std::clamp(128 + (v - 128) * (100 + amount) / 100, 0, 255);
        };
        return rgba(channel(red(c)), channel(green(c)), channel(blue(c)), alpha(c));
    }, selection, {layer.x, layer.y});
}
void boxBlur(Layer& layer, Rect area, int radius, const Selection* selection) {
    if (radius <= 0) return;
    const Plane source = layer.pixels;
    const auto extent = source.extent();
    if (extent.empty()) return;
    const auto affected = intersect(local(layer, area), {extent.x - radius, extent.y - radius,
        extent.w + 2 * radius, extent.h + 2 * radius});
    const int diameter = radius * 2 + 1;
    struct Sum {
        int r{}, g{}, b{}, a{};
        void add(Pixel p, int weight) {
            r += red(p) * weight; g += green(p) * weight; b += blue(p) * weight; a += alpha(p) * weight;
        }
        Pixel average(int divisor) const {
            return rgba((r + divisor / 2) / divisor, (g + divisor / 2) / divisor,
                (b + divisor / 2) / divisor, (a + divisor / 2) / divisor);
        }
    };
    std::vector<Pixel> horizontal(std::size_t(tileEdge) * (tileEdge + 2 * radius));
    std::vector<Pixel> result(tileEdge * tileEdge);
    eachTile(affected, [&](Coord, Rect patch) {
        for (int row = 0; row < patch.h + 2 * radius; ++row) {
            const int y = patch.y + row - radius;
            Sum sum;
            for (int i = -radius; i <= radius; ++i) sum.add(premultiply(source.pixel(patch.x + i, y)), 1);
            for (int x = 0; x < patch.w; ++x) {
                horizontal[row * patch.w + x] = sum.average(diameter);
                sum.add(premultiply(source.pixel(patch.x + x - radius, y)), -1);
                sum.add(premultiply(source.pixel(patch.x + x + radius + 1, y)), 1);
            }
        }
        for (int x = 0; x < patch.w; ++x) {
            Sum sum;
            for (int i = 0; i < diameter; ++i) sum.add(horizontal[i * patch.w + x], 1);
            for (int y = 0; y < patch.h; ++y) {
                result[y * patch.w + x] = straight(sum.average(diameter));
                if (y + 1 == patch.h) break;
                sum.add(horizontal[y * patch.w + x], -1);
                sum.add(horizontal[(y + diameter) * patch.w + x], 1);
            }
        }
        modify(layer.pixels, patch, false, [&](Pixel, int x, int y) {
            return result[std::size_t(y - patch.y) * patch.w + x - patch.x];
        }, selection, {layer.x, layer.y});
    });
}
Layer layerViaSelection(const Layer& source, const Selection& selection) {
    Layer copy = source;
    copy.pixels = Plane();
    copy.locked = false;
    const auto area = intersect(local(source, selection.bounds()), source.pixels.extent());
    eachTile(area, [&](Coord coord, Rect clip) {
        const auto tile = source.pixels.tiles().find(coord);
        if (tile == source.pixels.tiles().end()) return;
        bool covered = clip.w == tileEdge && clip.h == tileEdge;
        for (int y = clip.y; covered && y < clip.bottom(); ++y) {
            Run scratch;
            const auto runs = selection.row(y + source.y, scratch);
            const auto run = std::lower_bound(runs.begin(), runs.end(), clip.x + source.x,
                [](Run r, int x) { return r.last <= x; });
            covered = run != runs.end() && run->first <= clip.x + source.x && run->last >= clip.right() + source.x;
        }
        if (covered) {
            copy.pixels.assign(coord, tile->second);
        } else {
            modify(copy.pixels, clip, false, [&](Pixel, int x, int y) {
                return tile->second->px[(y - coord.y * tileEdge) * tileEdge + x - coord.x * tileEdge];
            }, &selection, {source.x, source.y});
        }
    });
    return copy;
}
void crop(State& state, Rect area) {
    area = intersect(area, state.bounds());
    if (area.empty()) return;
    for (auto& layer : state.layers) {
        layer.x -= area.x;
        layer.y -= area.y;
    }
    for (auto& guide : state.guides) guide.position -= guide.vertical ? area.x : area.y;
    std::erase_if(state.guides, [&](Guide guide) { return guide.position < 0 || guide.position > (guide.vertical ? area.w : area.h); });
    state.width = area.w;
    state.height = area.h;
}
void resizeCanvas(State& state, int width, int height) {
    const int x = (width - state.width) / 2, y = (height - state.height) / 2;
    for (auto& layer : state.layers) { layer.x += x; layer.y += y; }
    for (auto& guide : state.guides) guide.position += guide.vertical ? x : y;
    std::erase_if(state.guides, [&](Guide guide) { return guide.position < 0 || guide.position > (guide.vertical ? width : height); });
    state.width = width;
    state.height = height;
}
void flip(State& state, bool horizontal) {
    for (auto& guide : state.guides) if (guide.vertical == horizontal)
        guide.position = (horizontal ? state.width : state.height) - guide.position;
    for (auto& layer : state.layers) {
        auto mirror = [&](const Plane& source) {
            Plane result(source.background());
            std::unordered_map<const Tile*, std::shared_ptr<Tile>> checked;
            for (const auto& [coord, tile] : source.tiles()) {
                Coord to = horizontal ? Coord{-coord.x - 1, coord.y} : Coord{coord.x, -coord.y - 1};
                auto& target = checked[tile.get()];
                if (!target) {
                    target = std::make_shared<Tile>();
                    for (int y = 0; y < tileEdge; ++y) for (int x = 0; x < tileEdge; ++x) {
                        const int tx = horizontal ? tileEdge - 1 - x : x, ty = horizontal ? y : tileEdge - 1 - y;
                        target->px[ty * tileEdge + tx] = tile->px[y * tileEdge + x];
                    }
                }
                result.assign(to, target);
            }
            return result;
        };
        layer.pixels = mirror(layer.pixels);
        if (layer.mask) layer.mask = mirror(*layer.mask);
        if (horizontal) layer.x = state.width - layer.x;
        else layer.y = state.height - layer.y;
    }
}
void rotateClockwise(State& state) {
    for (auto& guide : state.guides) {
        if (!guide.vertical) guide.position = state.height - guide.position;
        guide.vertical = !guide.vertical;
    }
    for (auto& layer : state.layers) {
        auto rotate = [](const Plane& source) {
            Plane result(source.background());
            std::unordered_map<const Tile*, std::shared_ptr<Tile>> checked;
            for (const auto& [coord, tile] : source.tiles()) {
                auto& target = checked[tile.get()];
                if (!target) {
                    target = std::make_shared<Tile>();
                    for (int y = 0; y < tileEdge; ++y) for (int x = 0; x < tileEdge; ++x)
                        target->px[x * tileEdge + (tileEdge - 1 - y)] = tile->px[y * tileEdge + x];
                }
                result.assign({-coord.y - 1, coord.x}, target);
            }
            return result;
        };
        layer.pixels = rotate(layer.pixels);
        if (layer.mask) layer.mask = rotate(*layer.mask);
        const auto x = layer.x;
        layer.x = state.height - layer.y;
        layer.y = x;
    }
    std::swap(state.width, state.height);
}
void transformLayer(Layer& layer, Rect original, Rect destination, double degrees) {
    if (original.empty() || destination.empty() || destination.w > maxEdge || destination.h > maxEdge ||
        !std::isfinite(degrees) || std::abs(degrees) > 360)
        throw std::runtime_error("Invalid transform dimensions or angle.");
    if (!degrees && original.w == destination.w && original.h == destination.h) {
        layer.x += destination.x - original.x;
        layer.y += destination.y - original.y;
        return;
    }
    const InverseTransform inverse({original, destination, degrees});
    const auto [left, top, w, h] = inverse.extent();
    if (std::uint64_t(w) * h > maxDecodedPixels) throw std::runtime_error("The transformed layer exceeds 128 megapixels.");
    auto transform = [&](const Plane& source) {
        Plane out(source.background());
        if (source.tiles().empty()) return out;
        modify(out, {0, 0, w, h}, false, [&](Pixel before, int x, int y) {
            double px{}, py{};
            if (!inverse.map(x + left, y + top, px, py)) return before;
            return sampleLinear(source, px - layer.x, py - layer.y);
        });
        return out;
    };
    layer.pixels = transform(layer.pixels);
    if (layer.mask) layer.mask = transform(*layer.mask);
    layer.x = left;
    layer.y = top;
}

State resize(const State& source, int width, int height) {
    State out = source;
    out.width = width;
    out.height = height;
    const double sx = double(source.width) / width, sy = double(source.height) / height;
    for (auto& guide : out.guides) guide.position = int(std::round(guide.position / (guide.vertical ? sx : sy)));
    for (auto& layer : out.layers) {
        const int ox = layer.x, oy = layer.y;
        layer.x = 0;
        layer.y = 0;
        auto scale = [&](const Plane& input) {
            Plane result(input.background());
            auto extent = input.extent();
            if (extent.empty()) return result;
            extent = intersect({extent.x + ox - 1, extent.y + oy - 1, extent.w + 2, extent.h + 2}, source.bounds());
            if (extent.empty()) return result;
            const int left = int(std::floor(extent.x / sx)), top = int(std::floor(extent.y / sy));
            const int right = int(std::ceil(extent.right() / sx)), bottom = int(std::ceil(extent.bottom() / sy));
            auto area = intersect({left, top, right - left, bottom - top}, out.bounds());
            modify(result, area, false, [&](Pixel, int x, int y) {
                const double px = std::clamp((x + 0.5) * sx - 0.5, 0.0, source.width - 1.0) - ox;
                const double py = std::clamp((y + 0.5) * sy - 0.5, 0.0, source.height - 1.0) - oy;
                return sampleLinear(input, px, py);
            });
            return result;
        };
        const auto solid = ox == 0 && oy == 0 ? uniformColor(layer.pixels, source.width, source.height) : std::nullopt;
        layer.pixels = solid ? uniformPlane(width, height, *solid) : scale(layer.pixels);
        if (layer.mask) layer.mask = scale(*layer.mask);
    }
    return out;
}

void mergeVisible(State& state, bool flatten) {
    if (!flatten && std::none_of(state.layers.begin(), state.layers.end(), [](const Layer& layer) { return layer.visible; })) return;
    Layer merged;
    merged.name = flatten ? L"Background" : L"Merged";
    merged.locked = flatten;
    std::unordered_map<std::uint64_t, std::shared_ptr<Tile>> uniform;
    std::array<Pixel, tileEdge * tileEdge> buffer;
    eachTile(state.bounds(), [&](Coord coord, Rect area) {
        auto band = std::span(buffer).first(std::size_t(area.w) * area.h);
        composite(state, area, band, area.w);
        for (auto& p : band) {
            const int white = 255 - alpha(p);
            p = flatten ? rgba(red(p) + white, green(p) + white, blue(p) + white) : straight(p);
        }
        const auto first = band.front();
        if (std::all_of(band.begin(), band.end(), [&](Pixel p) { return p == first; })) {
            if (!first) return;
            const auto key = (std::uint64_t(first) << 16) | (std::uint64_t(area.w) << 8) | area.h;
            auto& tile = uniform[key];
            if (!tile) {
                tile = std::make_shared<Tile>();
                for (int y = 0; y < area.h; ++y) std::fill_n(tile->px.begin() + y * tileEdge, area.w, first);
            }
            merged.pixels.assign(coord, tile);
        } else importPixels(merged.pixels, area, band, area.w);
    });
    if (flatten) state.layers.clear();
    else std::erase_if(state.layers, [](const Layer& layer) { return layer.visible; });
    state.layers.push_back(std::move(merged));
    state.active = int(state.layers.size()) - 1;
}
bool mergeDown(State& state) {
    if (state.active <= 0) return false;
    const auto& lower = state.layers[state.active - 1];
    const auto& upper = state.layers[state.active];
    if (!lower.visible || !upper.visible || lower.adjustment || upper.adjustment ||
        lower.blend != Blend::normal || upper.blend != Blend::normal) return false;
    State pair{state.width, state.height, {lower, upper}, 1, state.ppi};
    mergeVisible(pair, false);
    auto layer = std::move(pair.layers.front());
    layer.name = lower.name;
    layer.locked = lower.locked;
    state.layers[state.active - 1] = std::move(layer);
    state.layers.erase(state.layers.begin() + state.active);
    --state.active;
    return true;
}

void importPixels(Plane& plane, Rect area, std::span<const Pixel> pixels, int stride) {
    modify(plane, area, false, [&](Pixel, int x, int y) {
        auto color = pixels[std::size_t(y - area.y) * stride + x - area.x];
        return alpha(color) ? color : 0;
    });
}
void composite(const State& state, Rect area, std::span<Pixel> pixels, int stride) {
    for (int y = 0; y < area.h; ++y) std::fill_n(pixels.data() + std::size_t(y) * stride, area.w, 0);
    for (const auto& layer : state.layers) {
        if (!layer.visible || !layer.opacity) continue;
        const Plane* maskPlane = layer.mask && layer.maskEnabled ? &*layer.mask : nullptr;
        const int defaultOpacity = mul255(layer.opacity, maskPlane ? red(maskPlane->background()) : 255);
        const Rect rel = local(layer, area);
        if (layer.adjustment) {
            const AdjustmentMap mapping(*layer.adjustment);
            eachTile(rel, [&](Coord coord, Rect clip) {
                const Tile* mask = maskPlane ? maskPlane->tile(coord) : nullptr;
                for (int y = clip.y; y < clip.bottom(); ++y) {
                    const int offset = (y - coord.y * tileEdge) * tileEdge + clip.x - coord.x * tileEdge;
                    auto* out = pixels.data() + std::size_t(y - rel.y) * stride + clip.x - rel.x;
                    for (int x = 0; x < clip.w; ++x) {
                        const int opacity = mask ? mul255(layer.opacity, red(mask->px[offset + x])) : defaultOpacity;
                        out[x] = mapping.apply(out[x], opacity, layer.blend);
                    }
                }
            });
            continue;
        }
        if (layer.pixels.tiles().empty()) continue;
        layerEffects(layer, area, 1, area.w, area.h, pixels, stride);
        eachTile(rel, [&](Coord coord, Rect clip) {
            auto tile = layer.pixels.tile(coord);
            if (!tile) return;
            const Tile* mask = maskPlane ? maskPlane->tile(coord) : nullptr;
            for (int y = clip.y; y < clip.bottom(); ++y) {
                const int offset = (y - coord.y * tileEdge) * tileEdge + clip.x - coord.x * tileEdge;
                auto out = pixels.data() + std::size_t(y - rel.y) * stride + clip.x - rel.x;
                const auto src = tile->px.data() + offset;
                if (mask) {
                    for (int x = 0; x < clip.w; ++x)
                        out[x] = over(out[x], styledPixel(src[x], layer), mul255(layer.opacity, red(mask->px[offset + x])), layer.blend);
                } else {
                    for (int x = 0; x < clip.w; ++x) out[x] = over(out[x], styledPixel(src[x], layer), defaultOpacity, layer.blend);
                }
            }
        });
    }
}
void preview(const State& state, Rect area, int step, int width, int height, std::span<Pixel> pixels,
    const TransformView* transform) {
    if (step == 1 && !transform) {
        composite(state, area, pixels, width);
        return;
    }
    std::fill(pixels.begin(), pixels.end(), 0);
    for (const auto& layer : state.layers) {
        if (!layer.visible || !layer.opacity || (!layer.adjustment && layer.pixels.tiles().empty())) continue;
        const Plane* maskPlane = layer.mask && layer.maskEnabled ? &*layer.mask : nullptr;
        const int defaultOpacity = mul255(layer.opacity, maskPlane ? red(maskPlane->background()) : 255);
        if (transform && &layer == &state.layers[state.active]) {
            const InverseTransform inverse(*transform);
            layerEffects(layer, area, step, width, height, pixels, width, &inverse);
            if (intersect(area, inverse.extent()).empty()) continue;
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const int gx = std::min(area.right() - 1, area.x + x * step + step / 2);
                const int gy = std::min(area.bottom() - 1, area.y + y * step + step / 2);
                double px{}, py{};
                if (!inverse.map(gx, gy, px, py)) continue;
                const int lx = int(std::round(px)) - layer.x, ly = int(std::round(py)) - layer.y;
                auto& out = pixels[std::size_t(y) * width + x];
                const int opacity = maskPlane ? mul255(layer.opacity, red(maskPlane->pixel(lx, ly))) : layer.opacity;
                out = over(out, styledPixel(layer.pixels.pixel(lx, ly), layer), opacity, layer.blend);
            }
            continue;
        }
        std::optional<AdjustmentMap> mapping;
        if (layer.adjustment) mapping.emplace(*layer.adjustment);
        else layerEffects(layer, area, step, width, height, pixels, width);
        for (int y = 0; y < height; ++y) {
            const int iy = std::min(area.bottom() - 1, area.y + y * step + step / 2) - layer.y;
            const int ty = tileIndex(iy);
            int previous = std::numeric_limits<int>::min();
            const Tile* tile = nullptr;
            const Tile* mask = nullptr;
            for (int x = 0; x < width; ++x) {
                const int ix = std::min(area.right() - 1, area.x + x * step + step / 2) - layer.x;
                const int tx = tileIndex(ix);
                if (tx != previous) {
                    previous = tx;
                    tile = layer.pixels.tile({tx, ty});
                    mask = maskPlane ? maskPlane->tile({tx, ty}) : nullptr;
                }
                if (!tile && !mapping) continue;
                const int i = (iy - ty * tileEdge) * tileEdge + ix - tx * tileEdge;
                auto& out = pixels[std::size_t(y) * width + x];
                const int opacity = mask ? mul255(layer.opacity, red(mask->px[i])) : defaultOpacity;
                out = mapping ? mapping->apply(out, opacity, layer.blend) :
                    over(out, styledPixel(tile->px[i], layer), opacity, layer.blend);
            }
        }
    }
}
} // namespace pf
