#include "document.hpp"

#include <cmath>
#include <limits>
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
    switch (mode) {
    case Blend::multiply: return mul255(s, d);
    case Blend::screen: return 255 - mul255(255 - s, 255 - d);
    case Blend::overlay: return d < 128 ? mul255(2 * d, s) : 255 - mul255(2 * (255 - d), 255 - s);
    case Blend::darken: return std::min(s, d);
    case Blend::lighten: return std::max(s, d);
    case Blend::difference: return std::abs(s - d);
    default: return s;
    }
}

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
void modify(Plane& plane, Rect area, bool existingOnly, F operation) {
    eachTile(area, [&](Coord coord, Rect clip) {
        const Tile* old = plane.tile(coord);
        if (existingOnly && !old) return;
        Tile* changed = nullptr;
        for (int y = clip.y; y < clip.bottom(); ++y) {
            for (int x = clip.x; x < clip.right(); ++x) {
                const int i = (y - coord.y * tileEdge) * tileEdge + x - coord.x * tileEdge;
                const Pixel before = changed ? changed->px[i] : old ? old->px[i] : plane.background();
                const Pixel after = operation(before, x, y);
                if (before == after) continue;
                if (!changed) changed = &plane.edit(coord);
                changed->px[i] = after;
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
    if (a.width != b.width || a.height != b.height || a.layers.size() != b.layers.size()) return false;
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        const auto& x = a.layers[i];
        const auto& y = b.layers[i];
        if (x.name != y.name || x.x != y.x || x.y != y.y || x.opacity != y.opacity || x.blend != y.blend ||
            x.visible != y.visible || x.locked != y.locked || x.pixels.identity() != y.pixels.identity() ||
            x.mask.has_value() != y.mask.has_value() ||
            (x.mask && x.mask->identity() != y.mask->identity())) return false;
    }
    return true;
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
    Pixel backdrop = mode == Blend::normal || da == 0 ? 0 : straight(d);
    auto channel = [&](int sc, int dc, int bc) {
        const int mixed = backdrop ? mul255(sc, 255 - da) + mul255(blendChannel(sc, bc, mode), da) : sc;
        return std::min(a, mul255(mixed, sa) + mul255(dc, 255 - sa));
    };
    return rgba(channel(red(s), red(d), red(backdrop)), channel(green(s), green(d), green(backdrop)),
        channel(blue(s), blue(d), blue(backdrop)), a);
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

Document::Document(State state, std::wstring label, std::size_t limit)
    : state_(std::move(state)), historyLimit_(limit) {
    if (state_.layers.empty()) state_.layers.emplace_back();
    state_.active = std::clamp(state_.active, 0, int(state_.layers.size()) - 1);
    history_.push_back({state_, std::move(label), 0, 0});
}
void Document::select(int index) {
    if (index >= 0 && index < int(state_.layers.size())) state_.active = index;
}
void Document::begin() {
    if (!baseline_) baseline_ = state_;
}
bool Document::commit(std::wstring label) {
    if (!baseline_) return false;
    for (auto& layer : state_.layers) {
        layer.pixels.prune();
        if (layer.mask) layer.mask->prune();
    }
    if (same(*baseline_, state_)) {
        baseline_.reset();
        return false;
    }
    const auto cost = estimate(*baseline_, state_);
    Entry entry{state_, std::move(label), nextId_, cost};
    history_.reserve(index_ + 2);
    while (history_.size() > index_ + 1) {
        historyBytes_ -= history_.back().cost;
        history_.pop_back();
    }
    history_.push_back(std::move(entry));
    ++nextId_;
    baseline_.reset();
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
    if (baseline_) state_ = std::move(*baseline_);
    baseline_.reset();
}
bool Document::jump(std::size_t position) {
    if (baseline_ || position >= history_.size() || position == index_) return false;
    const auto before = state_.bounds();
    state_ = State(history_[position].state);
    index_ = position;
    if (before.w != state_.width || before.h != state_.height) selection.reset();
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

Rect stamp(Layer& layer, Rect clip, float x, float y, const Brush& brush) {
    const float radius = brush.diameter * 0.5f;
    Rect region{int(std::floor(x - radius)), int(std::floor(y - radius)),
        int(std::ceil(x + radius) - std::floor(x - radius)), int(std::ceil(y + radius) - std::floor(y - radius))};
    region = intersect(region, clip);
    const float inner = std::max(0.0f, radius * brush.hardness - 0.5f);
    const float outer = radius + 0.35f;
    const int opacity = int(brush.opacity * 255 + 0.5f);
    auto& plane = brush.mask ? *layer.mask : layer.pixels;
    modify(plane, local(layer, region), brush.erase && !brush.mask, [&](Pixel old, int lx, int ly) {
        const float dx = lx + layer.x + 0.5f - x, dy = ly + layer.y + 0.5f - y;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= outer * outer) return old;
        const float coverage = d2 <= inner * inner ? 1.0f : (outer - std::sqrt(d2)) / (outer - inner);
        return paint(old, brush.color, int(coverage * opacity + 0.5f), brush.erase, brush.mask);
    });
    return region;
}
void fill(Layer& layer, Rect area, Pixel color, bool erase, bool mask) {
    auto& plane = mask ? *layer.mask : layer.pixels;
    modify(plane, local(layer, area), erase && !mask, [&](Pixel old, int, int) {
        return paint(old, color, 255, erase, mask);
    });
}
void gradient(Layer& layer, Rect area, Point from, Point to, Pixel start, Pixel end, bool mask) {
    const double dx = to.x - from.x, dy = to.y - from.y;
    const double length = std::max(1.0, dx * dx + dy * dy);
    auto& plane = mask ? *layer.mask : layer.pixels;
    modify(plane, local(layer, area), false, [&](Pixel old, int x, int y) {
        const double t = std::clamp(((x + layer.x - from.x) * dx + (y + layer.y - from.y) * dy) / length, 0.0, 1.0);
        auto mix = [t](int a, int b) { return int(a + (b - a) * t + 0.5); };
        return paint(old, rgba(mix(red(start), red(end)), mix(green(start), green(end)),
            mix(blue(start), blue(end)), mix(alpha(start), alpha(end))), 255, false, mask);
    });
}
void ellipse(Layer& layer, Rect area, Rect clip, Pixel color) {
    const double rx = area.w / 2.0, ry = area.h / 2.0;
    modify(layer.pixels, local(layer, intersect(area, clip)), false, [&](Pixel old, int x, int y) {
        const double dx = (x + layer.x + 0.5 - area.x - rx) / rx;
        const double dy = (y + layer.y + 0.5 - area.y - ry) / ry;
        const double edge = (1.0 - std::sqrt(dx * dx + dy * dy)) * std::min(rx, ry) + 0.5;
        return paint(old, color, int(std::clamp(edge, 0.0, 1.0) * 255), false, false);
    });
}
Rect flood(Layer& layer, Rect clip, Point point, Pixel color, int tolerance, bool mask) {
    if (!clip.contains(point)) return {};
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
        if (seen(x, y, false)) return false;
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
void adjust(Layer& layer, Rect area, Filter filter, int amount) {
    modify(layer.pixels, local(layer, area), true, [&](Pixel c, int, int) {
        if (!alpha(c)) return c;
        auto channel = [&](int v) {
            if (filter == Filter::invert) return 255 - v;
            if (filter == Filter::grayscale) return luminance(c);
            if (filter == Filter::brightness) return std::clamp(v + amount, 0, 255);
            return std::clamp(128 + (v - 128) * (100 + amount) / 100, 0, 255);
        };
        return rgba(channel(red(c)), channel(green(c)), channel(blue(c)), alpha(c));
    });
}
void boxBlur(Layer& layer, Rect area, int radius) {
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
        importPixels(layer.pixels, patch, result, patch.w);
    });
}
void crop(State& state, Rect area) {
    area = intersect(area, state.bounds());
    if (area.empty()) return;
    for (auto& layer : state.layers) {
        layer.x -= area.x;
        layer.y -= area.y;
    }
    state.width = area.w;
    state.height = area.h;
}
void flip(State& state, bool horizontal) {
    for (auto& layer : state.layers) {
        auto mirror = [&](const Plane& source) {
            Plane result(source.background());
            for (const auto& [coord, tile] : source.tiles()) {
                Coord to = horizontal ? Coord{-coord.x - 1, coord.y} : Coord{coord.x, -coord.y - 1};
                auto target = std::make_shared<Tile>();
                for (int y = 0; y < tileEdge; ++y) for (int x = 0; x < tileEdge; ++x) {
                    const int tx = horizontal ? tileEdge - 1 - x : x, ty = horizontal ? y : tileEdge - 1 - y;
                    target->px[ty * tileEdge + tx] = tile->px[y * tileEdge + x];
                }
                result.assign(to, std::move(target));
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
    for (auto& layer : state.layers) {
        auto rotate = [](const Plane& source) {
            Plane result(source.background());
            for (const auto& [coord, tile] : source.tiles()) {
                auto target = std::make_shared<Tile>();
                for (int y = 0; y < tileEdge; ++y) for (int x = 0; x < tileEdge; ++x)
                    target->px[x * tileEdge + (tileEdge - 1 - y)] = tile->px[y * tileEdge + x];
                result.assign({-coord.y - 1, coord.x}, std::move(target));
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
State resize(const State& source, int width, int height) {
    State out = source;
    out.width = width;
    out.height = height;
    const double sx = double(source.width) / width, sy = double(source.height) / height;
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
                const int ix = int(std::floor(px)), iy = int(std::floor(py));
                const double fx = px - ix, fy = py - iy;
                const Pixel a = premultiply(input.pixel(ix, iy)), b = premultiply(input.pixel(ix + 1, iy));
                const Pixel c = premultiply(input.pixel(ix, iy + 1)), d = premultiply(input.pixel(ix + 1, iy + 1));
                auto mix = [&](auto get) {
                    return int((1 - fy) * ((1 - fx) * get(a) + fx * get(b)) +
                        fy * ((1 - fx) * get(c) + fx * get(d)) + 0.5);
                };
                return straight(rgba(mix(red), mix(green), mix(blue), mix(alpha)));
            });
            return result;
        };
        layer.pixels = scale(layer.pixels);
        if (layer.mask) layer.mask = scale(*layer.mask);
    }
    return out;
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
        if (!layer.visible || !layer.opacity || layer.pixels.tiles().empty()) continue;
        const Rect rel = local(layer, area);
        eachTile(rel, [&](Coord coord, Rect clip) {
            auto tile = layer.pixels.tile(coord);
            if (!tile) return;
            const Tile* mask = layer.mask ? layer.mask->tile(coord) : nullptr;
            for (int y = clip.y; y < clip.bottom(); ++y) {
                const int offset = (y - coord.y * tileEdge) * tileEdge + clip.x - coord.x * tileEdge;
                auto out = pixels.data() + std::size_t(y - rel.y) * stride + clip.x - rel.x;
                const auto src = tile->px.data() + offset;
                if (mask) {
                    for (int x = 0; x < clip.w; ++x)
                        out[x] = over(out[x], src[x], mul255(layer.opacity, red(mask->px[offset + x])), layer.blend);
                } else {
                    for (int x = 0; x < clip.w; ++x) out[x] = over(out[x], src[x], layer.opacity, layer.blend);
                }
            }
        });
    }
}
void preview(const State& state, Rect area, int step, int width, int height, std::span<Pixel> pixels) {
    if (step == 1) {
        composite(state, area, pixels, width);
        return;
    }
    std::fill(pixels.begin(), pixels.end(), 0);
    for (const auto& layer : state.layers) {
        if (!layer.visible || !layer.opacity || layer.pixels.tiles().empty()) continue;
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
                    mask = layer.mask ? layer.mask->tile({tx, ty}) : nullptr;
                }
                if (!tile) continue;
                const int i = (iy - ty * tileEdge) * tileEdge + ix - tx * tileEdge;
                auto& out = pixels[std::size_t(y) * width + x];
                const int opacity = mask ? mul255(layer.opacity, red(mask->px[i])) : layer.opacity;
                out = over(out, tile->px[i], opacity, layer.blend);
            }
        }
    }
}
} // namespace pf
