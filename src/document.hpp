#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace pf {

using Pixel = std::uint32_t; // Straight BGRA, with alpha in the high byte.
inline constexpr int tileEdge = 128;
inline constexpr int maxEdge = 16384;
inline constexpr std::uint64_t maxDecodedPixels = 128'000'000;
inline constexpr int maxLayers = 256;

struct Point { int x{}, y{}; };
struct Rect {
    int x{}, y{}, w{}, h{};
    bool empty() const { return w <= 0 || h <= 0; }
    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool contains(Point p) const { return p.x >= x && p.x < right() && p.y >= y && p.y < bottom(); }
};
Rect intersect(Rect a, Rect b);
Rect unite(Rect a, Rect b);
Rect between(Point a, Point b);
int tileIndex(int coordinate);

constexpr Pixel rgba(int r, int g, int b, int a = 255) {
    return (Pixel(a) << 24) | (Pixel(r) << 16) | (Pixel(g) << 8) | Pixel(b);
}
constexpr int red(Pixel c) { return (c >> 16) & 255; }
constexpr int green(Pixel c) { return (c >> 8) & 255; }
constexpr int blue(Pixel c) { return c & 255; }
constexpr int alpha(Pixel c) { return c >> 24; }
Pixel premultiply(Pixel c);
Pixel straight(Pixel c);

enum class Blend : std::uint8_t { normal, multiply, screen, overlay, darken, lighten, difference };
Pixel over(Pixel premulDst, Pixel src, int opacity = 255, Blend mode = Blend::normal);

struct Coord {
    int x{}, y{};
    bool operator==(const Coord&) const = default;
};
struct CoordHash { std::size_t operator()(Coord c) const; };
struct Tile { std::array<Pixel, tileEdge * tileEdge> px{}; };

class Plane {
public:
    using Tiles = std::unordered_map<Coord, std::shared_ptr<Tile>, CoordHash>;
    Plane() = default;
    explicit Plane(Pixel background) : background_(background) {}
    Pixel pixel(int x, int y) const;
    const Tile* tile(Coord c) const;
    Tile& edit(Coord c);
    void assign(Coord c, std::shared_ptr<Tile> tile);
    void prune();
    Rect extent() const;
    const Tiles& tiles() const { return *tiles_; }
    const void* identity() const { return tiles_.get(); }
    Pixel background() const { return background_; }

private:
    void detach();
    Pixel background_{};
    std::shared_ptr<Tiles> tiles_ = std::make_shared<Tiles>();
};

struct Layer {
    std::wstring name = L"Layer";
    Plane pixels;
    std::optional<Plane> mask;
    int x{}, y{};
    int opacity = 255;
    Blend blend = Blend::normal;
    bool visible = true;
    bool locked = false;
};
struct State {
    int width{}, height{};
    std::vector<Layer> layers; // Bottom to top.
    int active{};
    Rect bounds() const { return {0, 0, width, height}; }
};

class Document {
public:
    explicit Document(State state, std::wstring label = L"Open", std::size_t historyLimit = 96ull << 20);
    const State& state() const { return state_; }
    State& edit() { return state_; }
    Layer& active() { return state_.layers[state_.active]; }
    const Layer& active() const { return state_.layers[state_.active]; }

    void select(int index);
    void begin();
    bool commit(std::wstring label);
    void cancel();
    bool jump(std::size_t position);
    bool undo();
    bool redo();
    bool inEdit() const { return baseline_.has_value(); }
    std::size_t historyIndex() const { return index_; }
    std::size_t historyCount() const { return history_.size(); }
    const std::wstring& historyLabel(std::size_t i) const { return history_[i].label; }
    bool dirty() const { return currentId() != savedId_; }
    void markSaved() { savedId_ = currentId(); }
    std::size_t pixelBytes() const;
    std::size_t historyBytes() const { return historyBytes_; }

    std::optional<Rect> selection;

private:
    struct Entry { State state; std::wstring label; std::uint64_t id; std::size_t cost; };
    std::uint64_t currentId() const { return history_[index_].id; }
    State state_;
    std::optional<State> baseline_;
    std::vector<Entry> history_;
    std::size_t historyLimit_, historyBytes_{}, index_{};
    std::uint64_t nextId_ = 1, savedId_{};
};

struct Brush {
    Pixel color = rgba(229, 157, 98);
    float diameter = 48;
    float hardness = 0.80f;
    float opacity = 1;
    bool erase = false;
    bool mask = false;
};

Rect stamp(Layer& layer, Rect clip, float x, float y, const Brush& brush);
void fill(Layer& layer, Rect area, Pixel color, bool erase = false, bool mask = false);
void gradient(Layer& layer, Rect area, Point from, Point to, Pixel start, Pixel end, bool mask = false);
void ellipse(Layer& layer, Rect area, Rect clip, Pixel color);
Rect flood(Layer& layer, Rect clip, Point point, Pixel color, int tolerance, bool mask = false);
enum class Filter { invert, grayscale, brightness, contrast };
void adjust(Layer& layer, Rect area, Filter filter, int amount = 0);
void boxBlur(Layer& layer, Rect area, int radius);
void crop(State& state, Rect area);
void flip(State& state, bool horizontal);
void rotateClockwise(State& state);
State resize(const State& source, int width, int height);
void importPixels(Plane& plane, Rect area, std::span<const Pixel> pixels, int stride);
void composite(const State& state, Rect area, std::span<Pixel> pixels, int stride);
void preview(const State& state, Rect area, int step, int width, int height, std::span<Pixel> pixels);

} // namespace pf
