#include "document.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace pf {
namespace {
constexpr std::size_t maxRuns = 4'000'000;

class Rows {
public:
    explicit Rows(Rect area) : area_(area) { offsets_.reserve(std::size_t(area.h) + 1); }
    void append(Run run) {
        if (run.first >= run.last) return;
        if (!offsets_.empty() && spans_.size() > offsets_.back() && spans_.back().last >= run.first) {
            spans_.back().last = std::max(spans_.back().last, run.last);
            return;
        }
        if (spans_.size() >= maxRuns) throw std::runtime_error("The selection contains too many disconnected areas.");
        spans_.push_back(run);
    }
    void startRow() { offsets_.push_back(std::uint32_t(spans_.size())); }
    Selection finish() {
        startRow();
        return Selection::fromRows(area_, std::move(offsets_), std::move(spans_));
    }
private:
    Rect area_;
    std::vector<std::uint32_t> offsets_;
    std::vector<Run> spans_;
};

template<class F>
void difference(std::span<const Run> runs, std::span<const Run> cut, F add) {
    std::size_t j = 0;
    for (const auto run : runs) {
        int left = run.first;
        while (j < cut.size() && cut[j].last <= left) ++j;
        for (; j < cut.size() && cut[j].first < run.last; ++j) {
            if (cut[j].first > left) add(Run{left, cut[j].first});
            left = std::max(left, cut[j].last);
            if (left >= run.last) break;
        }
        if (left < run.last) add(Run{left, run.last});
    }
}

bool matches(Pixel a, Pixel b, int tolerance) {
    return std::max({std::abs(red(a) - red(b)), std::abs(green(a) - green(b)),
        std::abs(blue(a) - blue(b)), std::abs(alpha(a) - alpha(b))}) <= tolerance;
}
} // namespace

std::span<const Run> Selection::row(int y, Run& rectangular) const {
    if (y < bounds_.y || y >= bounds_.bottom() || bounds_.empty()) return {};
    if (!rows_) {
        rectangular = {bounds_.x, bounds_.right()};
        return {&rectangular, 1};
    }
    const auto index = std::size_t(y - rows_->top);
    return std::span(rows_->spans).subspan(rows_->offsets[index], rows_->offsets[index + 1] - rows_->offsets[index]);
}
std::size_t Selection::memoryBytes() const {
    return rows_ ? sizeof(Rows) + rows_->offsets.capacity() * sizeof(std::uint32_t) + rows_->spans.capacity() * sizeof(Run) : 0;
}

bool Selection::contains(Point p) const {
    if (!bounds_.contains(p)) return false;
    if (!rows_) return true;
    Run storage;
    const auto runs = row(p.y, storage);
    const auto run = std::lower_bound(runs.begin(), runs.end(), p.x, [](Run span, int x) { return span.last <= x; });
    return run != runs.end() && run->first <= p.x;
}

Selection Selection::fromRows(Rect area, std::vector<std::uint32_t> offsets, std::vector<Run> spans) {
    if (spans.empty()) return {};
    int left = spans.front().first, right = spans.front().last;
    for (const auto run : spans) { left = std::min(left, run.first); right = std::max(right, run.last); }
    int top = 0, bottom = area.h;
    while (offsets[top] == offsets[top + 1]) ++top;
    while (offsets[bottom] == offsets[bottom - 1]) --bottom;
    Selection selection({left, area.y + top, right - left, bottom - top});
    const bool uniform = std::size_t(bottom - top) == spans.size() &&
        std::all_of(spans.begin(), spans.end(), [&](Run run) { return run == Run{left, right}; }) &&
        std::adjacent_find(offsets.begin() + top, offsets.begin() + bottom + 1) == offsets.begin() + bottom + 1;
    if (!uniform) selection.rows_ = std::make_shared<Selection::Rows>(Selection::Rows{area.y, std::move(offsets), std::move(spans)});
    return selection;
}

std::vector<Edge> Selection::outline() const {
    if (empty()) return {};
    const auto [x, y, w, h] = bounds_;
    auto rectangle = [&] {
        return std::vector<Edge>{{{x, y}, {x + w, y}}, {{x + w, y}, {x + w, y + h}},
            {{x + w, y + h}, {x, y + h}}, {{x, y + h}, {x, y}}};
    };
    if (!rows_) return rectangle();
    constexpr std::size_t limit = 150'000;
    std::vector<Edge> edges;
    for (int yy = y; yy < y + h; ++yy) {
        Run scratch, above, below;
        const auto spans = row(yy, scratch);
        for (const auto run : spans) {
            edges.push_back({{run.first, yy}, {run.first, yy + 1}});
            edges.push_back({{run.last, yy}, {run.last, yy + 1}});
        }
        difference(spans, row(yy - 1, above), [&](Run run) { edges.push_back({{run.first, yy}, {run.last, yy}}); });
        difference(spans, row(yy + 1, below), [&](Run run) { edges.push_back({{run.first, yy + 1}, {run.last, yy + 1}}); });
        if (edges.size() > limit) return rectangle();
    }
    return edges;
}

Selection ellipseSelection(Rect ellipse, Rect clip) {
    const auto area = intersect(ellipse, clip);
    if (area.empty()) return {};
    Rows rows(area);
    const double rx = ellipse.w / 2.0, ry = ellipse.h / 2.0;
    const double cx = ellipse.x + rx, cy = ellipse.y + ry;
    for (int y = area.y; y < area.bottom(); ++y) {
        rows.startRow();
        const auto v = (y + 0.5 - cy) / ry;
        const auto extent = rx * std::sqrt(std::max(0.0, 1 - v * v));
        rows.append({std::max(area.x, int(std::ceil(cx - extent - 0.5))),
            std::min(area.right(), int(std::floor(cx + extent - 0.5)) + 1)});
    }
    return rows.finish();
}

Selection lassoSelection(std::span<const Point> path, Rect clip) {
    if (path.size() < 3) return {};
    Rect bounds;
    for (const auto p : path) bounds = unite(bounds, {p.x, p.y, 1, 1});
    const auto area = intersect(bounds, clip);
    if (area.empty()) return {};
    struct Crossing { int start, end; double x, slope; };
    std::vector<Crossing> edges;
    for (std::size_t i = 0; i < path.size(); ++i) {
        auto a = path[i], b = path[(i + 1) % path.size()];
        if (a.y == b.y) continue;
        if (a.y > b.y) std::swap(a, b);
        const int start = std::max(a.y, area.y), end = std::min(b.y, area.bottom());
        if (start >= end) continue;
        const double slope = double(b.x - a.x) / (b.y - a.y);
        edges.push_back({start, end, a.x + slope * (start + 0.5 - a.y), slope});
    }
    std::sort(edges.begin(), edges.end(), [](const Crossing& a, const Crossing& b) { return a.start < b.start; });
    std::vector<Crossing> active;
    std::vector<double> xs;
    std::size_t next = 0;
    Rows rows(area);
    for (int y = area.y; y < area.bottom(); ++y) {
        rows.startRow();
        std::erase_if(active, [&](const Crossing& edge) { return edge.end <= y; });
        for (; next < edges.size() && edges[next].start == y; ++next) active.push_back(edges[next]);
        xs.clear();
        for (auto& edge : active) { xs.push_back(edge.x); edge.x += edge.slope; }
        std::sort(xs.begin(), xs.end());
        for (std::size_t i = 0; i + 1 < xs.size(); i += 2)
            rows.append({std::max(area.x, int(std::ceil(xs[i] - 0.5))),
                std::min(area.right(), int(std::ceil(xs[i + 1] - 0.5)))});
    }
    return rows.finish();
}

Selection combineSelection(const Selection& previous, const Selection& incoming, SelectMode mode) {
    if (mode == SelectMode::replace || (previous.empty() && mode == SelectMode::add)) return incoming;
    if (incoming.empty()) return mode == SelectMode::intersect ? Selection{} : previous;
    const auto bounds = mode == SelectMode::add ? unite(previous.bounds(), incoming.bounds()) :
        mode == SelectMode::subtract ? previous.bounds() : intersect(previous.bounds(), incoming.bounds());
    if (bounds.empty()) return {};
    Rows rows(bounds);
    for (int y = bounds.y; y < bounds.bottom(); ++y) {
        rows.startRow();
        Run ra, rb;
        const auto a = previous.row(y, ra), b = incoming.row(y, rb);
        if (mode == SelectMode::subtract) {
            difference(a, b, [&](Run run) { rows.append(run); });
        } else if (mode == SelectMode::add) {
            auto ia = a.begin(), ib = b.begin();
            while (ia != a.end() || ib != b.end()) {
                if (ib == b.end() || (ia != a.end() && ia->first <= ib->first)) rows.append(*ia++);
                else rows.append(*ib++);
            }
        } else {
            auto ia = a.begin(), ib = b.begin();
            while (ia != a.end() && ib != b.end()) {
                rows.append({std::max(ia->first, ib->first), std::min(ia->last, ib->last)});
                if (ia->last < ib->last) ++ia;
                else ++ib;
            }
        }
    }
    return rows.finish();
}

Selection selectLayerPixels(const Layer& layer, Rect canvas) {
    auto extent = layer.pixels.extent();
    extent.x += layer.x;
    extent.y += layer.y;
    const auto area = intersect(canvas, extent);
    if (area.empty()) return {};
    Rows rows(area);
    for (int y = area.y; y < area.bottom(); ++y) {
        rows.startRow();
        const auto ly = y - layer.y, ty = tileIndex(ly);
        for (int x = area.x; x < area.right();) {
            const auto lx = x - layer.x, tx = tileIndex(lx);
            const auto end = std::min(area.right(), (tx + 1) * tileEdge + layer.x);
            if (const auto tile = layer.pixels.tile({tx, ty})) {
                const auto row = tile->px.data() + (ly - ty * tileEdge) * tileEdge;
                int start = x;
                for (int i = x; i < end; ++i) {
                    if (alpha(row[i - layer.x - tx * tileEdge])) continue;
                    rows.append({start, i});
                    start = i + 1;
                }
                rows.append({start, end});
            }
            x = end;
        }
    }
    return rows.finish();
}

Selection wandSelection(const State& state, Point source, int tolerance, bool contiguous, bool merged) {
    const auto area = state.bounds();
    if (!area.contains(source)) return {};
    const auto& layer = state.layers[state.active];
    if (!merged && layer.pixels.tiles().empty()) return Selection(area);
    std::array<Pixel, 1> sample{};
    if (merged) composite(state, {source.x, source.y, 1, 1}, sample, 1);
    const auto target = merged ? straight(sample.front()) : layer.pixels.pixel(source.x - layer.x, source.y - layer.y);
    const int stride = (area.w + 63) / 64;
    std::vector<std::uint64_t> candidates(contiguous ? std::size_t(stride) * area.h : 0);
    constexpr int batch = 24;
    std::vector<Pixel> pixels(merged ? std::size_t(area.w) * batch : 0);
    Rows rows(area);
    for (int top = 0; top < area.h; top += batch) {
        const int h = std::min(batch, area.h - top);
        if (merged) composite(state, {0, top, area.w, h}, pixels, area.w);
        for (int y = top; y < top + h; ++y) {
            if (!contiguous) rows.startRow();
            int start = -1;
            const Tile* tile = nullptr;
            int tileX = std::numeric_limits<int>::min();
            const auto ly = y - layer.y, ty = tileIndex(ly);
            for (int x = 0; x < area.w; ++x) {
                Pixel color;
                if (merged) color = straight(pixels[std::size_t(y - top) * area.w + x]);
                else {
                    const auto lx = x - layer.x, tx = tileIndex(lx);
                    if (tileX != tx) { tileX = tx; tile = layer.pixels.tile({tx, ty}); }
                    color = tile ? tile->px[(ly - ty * tileEdge) * tileEdge + lx - tx * tileEdge] : 0;
                }
                const bool match = matches(color, target, tolerance);
                if (contiguous) {
                    if (match) candidates[std::size_t(y) * stride + x / 64] |= 1ull << (x & 63);
                } else if (match) {
                    if (start < 0) start = x;
                } else if (start >= 0) {
                    rows.append({start, x});
                    start = -1;
                }
            }
            if (!contiguous && start >= 0) rows.append({start, area.w});
        }
    }
    if (!contiguous) return rows.finish();

    auto match = [&](int x, int y) { return (candidates[std::size_t(y) * stride + x / 64] >> (x & 63)) & 1ull; };
    struct Selected { int y; Run run; };
    std::vector<Selected> selected;
    std::vector<Point> pending{source};
    while (!pending.empty()) {
        const auto [x, y] = pending.back();
        pending.pop_back();
        if (!match(x, y)) continue;
        int left = x, right = x + 1;
        while (left > 0 && match(left - 1, y)) --left;
        while (right < area.w && match(right, y)) ++right;
        for (int xx = left; xx < right; ++xx) candidates[std::size_t(y) * stride + xx / 64] &= ~(1ull << (xx & 63));
        if (selected.size() >= maxRuns) throw std::runtime_error("The selection contains too many disconnected areas.");
        selected.push_back({y, {left, right}});
        for (const int next : {y - 1, y + 1}) {
            if (next < 0 || next >= area.h) continue;
            bool inside = false;
            for (int xx = left; xx < right; ++xx) {
                const bool selected = match(xx, next) != 0;
                if (selected && !inside) {
                    if (pending.size() >= maxRuns) throw std::runtime_error("The selection contains too many disconnected areas.");
                    pending.push_back({xx, next});
                }
                inside = selected;
            }
        }
    }
    std::sort(selected.begin(), selected.end(), [](const Selected& a, const Selected& b) {
        return a.y == b.y ? a.run.first < b.run.first : a.y < b.y;
    });
    std::size_t next = 0;
    for (int y = 0; y < area.h; ++y) {
        rows.startRow();
        for (; next < selected.size() && selected[next].y == y; ++next) rows.append(selected[next].run);
    }
    return rows.finish();
}
} // namespace pf
