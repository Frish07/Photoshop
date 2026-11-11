#include "formats.hpp"
#include "binary.hpp"

#include <cmath>
#include <numeric>

namespace pf {
namespace {
constexpr std::array<std::string_view, 7> blends{"norm", "mul ", "scrn", "over", "dark", "lite", "diff"};
constexpr auto psdLimit = 2ull * 1024 * 1024 * 1024 - 1;

struct Channel {
    int id{};
    std::uint32_t size{};
    std::uint64_t offset{};
};
struct Record {
    Layer layer;
    Rect bounds;
    std::vector<Channel> channels;
    bool complex{};
};

int signed16(std::uint16_t value) { return value >= 32768 ? int(value) - 65536 : value; }

std::uint64_t section(Reader& file, std::uint64_t length, std::uint64_t end) {
    require(file.pos() <= end && length <= end - file.pos(), "Invalid PSD section length.");
    return file.pos() + length;
}
double readResolution(Reader& in) {
    const auto end = section(in, in.u32(), in.limit());
    double ppi = 72;
    while (in.pos() + 12 <= end && in.str(4) == "8BIM") {
        const auto id = in.u16();
        const auto name = in.u8();
        in.seek(section(in, name + (name % 2 == 0), end));
        const auto size = in.u32();
        const auto blockEnd = section(in, size, end);
        if (id == 1005 && size >= 16) {
            const auto resolution = in.u32() / 65536.0;
            if (resolution >= 1 && resolution <= 2400) ppi = resolution;
        }
        in.seek(blockEnd);
        if (size % 2 && in.pos() < end) in.skip(1);
    }
    in.seek(end);
    return ppi;
}
Rect rectangle(Reader& in) {
    const int top = in.i32(), left = in.i32(), bottom = in.i32(), right = in.i32();
    require(top >= -maxEdge * 4 && left >= -maxEdge * 4 && right <= maxEdge * 4 &&
        bottom <= maxEdge * 4 && bottom >= top && right >= left, "Invalid PSD layer rectangle.");
    return {left, top, right - left, bottom - top};
}
Record readRecord(Reader& in, std::uint64_t end) {
    Record rec;
    rec.bounds = rectangle(in);
    const auto channels = in.u16();
    require(channels <= 56, "Invalid PSD channel count.");
    for (int i = 0; i < channels; ++i) rec.channels.push_back({signed16(in.u16()), in.u32()});
    require(in.str(4) == "8BIM", "Invalid PSD blend signature.");
    const auto name = in.str(4);
    const auto mode = std::find(blends.begin(), blends.end(), name);
    rec.complex = mode == blends.end();
    if (!rec.complex) rec.layer.blend = Blend(mode - blends.begin());
    rec.layer.opacity = in.u8();
    if (in.u8() != 0) rec.complex = true; // Clipping layer.
    const auto flags = in.u8();
    rec.layer.visible = (flags & 2) == 0;
    in.skip(1);
    auto extra = section(in, in.u32(), end);
    auto mask = section(in, in.u32(), extra);
    if (mask != in.pos()) rec.complex = true;
    in.seek(mask);
    auto ranges = section(in, in.u32(), extra);
    in.seek(ranges);
    auto start = in.pos();
    const auto n = in.u8();
    rec.layer.name.clear();
    for (int i = 0; i < n; ++i) rec.layer.name += wchar_t(in.u8());
    in.seek(section(in, ((n + 1 + 3) & ~3) - (in.pos() - start), extra));
    while (in.pos() + 12 <= extra) {
        const auto signature = in.str(4);
        if (signature != "8BIM" && signature != "8B64") break;
        const auto key = in.str(4);
        const auto length = in.u32();
        const auto dataEnd = section(in, length, extra);
        if (key == "luni" && length >= 4) {
            const auto chars = in.u32();
            require(chars <= (length - 4) / 2 && chars <= 32767, "Invalid PSD Unicode layer name.");
            rec.layer.name.clear();
            for (std::uint32_t i = 0; i < chars; ++i) {
                const auto ch = in.u16();
                if (ch) rec.layer.name += wchar_t(ch);
            }
        }
        if (key == "lspf" && length >= 4) rec.layer.locked = (in.u32() & 7) != 0;
        constexpr std::array<std::string_view, 26> complexKeys{"lsct", "lsdk", "TySh", "tySh", "SoLd", "SoLE",
            "PlLd", "vmsk", "vsms", "vogk", "vstk", "lfx2", "lrFX", "levl", "curv", "brit", "hue2", "blnc",
            "vibA", "expA", "selc", "mixr", "grdm", "SoCo", "GdFl", "PtFl"};
        rec.complex |= std::find(complexKeys.begin(), complexKeys.end(), key) != complexKeys.end();
        in.seek(dataEnd);
        if (length % 2 && in.pos() < extra) in.skip(1);
    }
    in.seek(extra);
    rec.layer.x = rec.bounds.x; rec.layer.y = rec.bounds.y;
    return rec;
}

struct PlaneReader {
    std::uint64_t begin{};
    std::vector<std::uint64_t> rows;
    bool rle{};
};
PlaneReader table(Reader& in, int height, int width, int compression, std::uint64_t end) {
    PlaneReader result;
    result.rle = compression == 1;
    result.begin = in.pos();
    if (!result.rle) {
        section(in, std::uint64_t(width) * height, end);
        return result;
    }
    result.rows.resize(std::size_t(height) + 1);
    return result;
}
void unpack(std::span<const std::uint8_t> input, std::span<std::uint8_t> out) {
    std::size_t i = 0, j = 0;
    while (i < input.size() && j < out.size()) {
        const int token = input[i++];
        if (token < 128) {
            const auto length = std::size_t(token + 1);
            require(i + length <= input.size() && j + length <= out.size(), "Invalid PSD RLE literal.");
            std::copy_n(input.data() + i, length, out.data() + j);
            i += length; j += length;
        } else if (token > 128) {
            const auto length = std::size_t(257 - token);
            require(i < input.size() && j + length <= out.size(), "Invalid PSD RLE run.");
            std::fill_n(out.data() + j, length, input[i++]);
            j += length;
        }
    }
    require(j == out.size(), "Incomplete PSD scanline.");
}
void loadBands(Reader& in, Plane& destination, int width, int height,
    const std::array<std::optional<PlaneReader>, 4>& channels, bool gray) {
    constexpr int batch = 24;
    std::vector<std::uint8_t> band(std::size_t(width) * batch);
    std::vector<std::uint8_t> packed;
    std::vector<Pixel> pixels(std::size_t(width) * batch);
    for (int y = 0; y < height; y += batch) {
        const int lines = std::min(batch, height - y);
        const int count = width * lines;
        std::fill_n(pixels.begin(), count, 0xff000000u);
        for (int c = 0; c < 4; ++c) {
            if (!channels[c]) continue;
            const auto& channel = *channels[c];
            if (!channel.rle) {
                in.seek(channel.begin + std::uint64_t(y) * width);
                in.bytes(band.data(), count);
            } else {
                in.seek(channel.rows[y]);
                for (int row = 0; row < lines; ++row) {
                    const auto len = channel.rows[y + row + 1] - channel.rows[y + row];
                    packed.resize(std::size_t(len));
                    in.bytes(packed.data(), packed.size());
                    unpack(packed, std::span(band).subspan(std::size_t(row) * width, width));
                }
            }
            for (int i = 0; i < count; ++i) {
                if (gray && c == 0) pixels[i] |= Pixel(band[i]) * 0x010101;
                else if (c == 3) pixels[i] = (pixels[i] & 0xffffffu) | (Pixel(band[i]) << 24);
                else pixels[i] |= Pixel(band[i]) << (16 - c * 8);
            }
        }
        importPixels(destination, {0, y, width, lines}, pixels, width);
    }
}
void loadLayer(Reader& in, Record& rec, bool gray) {
    const auto [x, y, width, height] = rec.bounds;
    if (!width || !height) return;
    std::array<std::optional<PlaneReader>, 4> sources;
    for (const auto& ch : rec.channels) {
        const int c = ch.id == -1 ? 3 : ch.id;
        if (c < 0 || c > (gray ? 0 : 2)) {
            if (ch.id != -1) continue;
        }
        require(ch.size >= 2, "Invalid PSD layer channel.");
        in.seek(ch.offset);
        const int compression = in.u16();
        require(compression < 2, "This PSD uses ZIP-compressed layers. Save with a compatible merged preview or use 8-bit RLE.");
        auto source = table(in, height, width, compression, ch.offset + ch.size);
        if (source.rle) {
            section(in, std::uint64_t(height) * 2, ch.offset + ch.size);
            auto offset = in.pos() + std::uint64_t(height) * 2;
            for (int row = 0; row < height; ++row) {
                source.rows[row] = offset;
                offset += in.u16();
            }
            require(offset <= ch.offset + ch.size, "Invalid PSD RLE offsets.");
            source.rows[height] = offset;
        }
        sources[c] = std::move(source);
    }
    loadBands(in, rec.layer.pixels, width, height, sources, gray);
}
State merged(Reader& in, State state, int channels, bool gray, bool hasAlpha) {
    const int compression = in.u16();
    require(compression < 2, "This PSD uses ZIP compression for the merged preview.");
    const int colorCount = gray ? 1 : 3;
    const int wanted = std::min(channels, colorCount + int(hasAlpha));
    std::array<std::optional<PlaneReader>, 4> sources;
    if (compression == 0) {
        const auto size = std::uint64_t(state.width) * state.height;
        for (int c = 0; c < wanted; ++c) {
            sources[c == colorCount ? 3 : c] = table(in, state.height, state.width, 0, in.limit());
            in.skip(size);
        }
    } else {
        const auto lengths = std::uint64_t(channels) * state.height * 2;
        auto offset = section(in, lengths, in.limit());
        for (int c = 0; c < wanted; ++c) {
            PlaneReader src;
            src.rle = true;
            src.rows.resize(std::size_t(state.height) + 1);
            for (int y = 0; y < state.height; ++y) {
                src.rows[y] = offset;
                offset += in.u16();
            }
            require(offset <= in.limit(), "Invalid merged PSD scanline offsets.");
            src.rows[state.height] = offset;
            sources[c == colorCount ? 3 : c] = std::move(src);
        }
    }
    Layer layer;
    layer.name = L"Merged PSD";
    loadBands(in, layer.pixels, state.width, state.height, sources, gray);
    state.layers.push_back(std::move(layer));
    return state;
}

std::size_t psdNameLength(const Layer& layer) { return std::min<std::size_t>(255, layer.name.size()); }
std::uint32_t extraSize(const Layer& layer) {
    const auto pascal = (psdNameLength(layer) + 4) & ~std::size_t(3);
    return std::uint32_t(8 + pascal + 12 + 4 + layer.name.size() * 2 + (layer.locked ? 16 : 0));
}
void writeRecord(Writer& out, const Layer& layer, Rect bounds) {
    out.u32(bounds.y); out.u32(bounds.x); out.u32(bounds.bottom()); out.u32(bounds.right());
    out.u16(4);
    for (int id : {-1, 0, 1, 2}) {
        out.u16(std::uint16_t(id));
        out.u32(2 + std::uint32_t(bounds.w) * bounds.h);
    }
    out.str("8BIM");
    out.str(blends[std::size_t(layer.blend)]);
    out.u8(std::uint8_t(layer.opacity)); out.u8(0); out.u8(layer.visible ? 0 : 2); out.u8(0);
    out.u32(extraSize(layer));
    out.u32(0); out.u32(0);
    const auto n = psdNameLength(layer);
    out.u8(std::uint8_t(n));
    for (std::size_t i = 0; i < n; ++i) out.u8(layer.name[i] < 128 ? std::uint8_t(layer.name[i]) : '?');
    for (std::size_t i = n + 1; i % 4; ++i) out.u8(0);
    out.str("8BIMluni"); out.u32(std::uint32_t(4 + layer.name.size() * 2));
    out.u32(std::uint32_t(layer.name.size()));
    for (auto ch : layer.name) out.u16(std::uint16_t(ch));
    if (layer.locked) { out.str("8BIMlspf"); out.u32(4); out.u32(7); }
}
} // namespace

State loadPsd(const std::filesystem::path& path) {
    Reader in(path);
    require(in.str(4) == "8BPS" && in.u16() == 1, "Only standard PSD files are supported.");
    in.skip(6);
    const int channels = in.u16();
    State state;
    state.height = in.i32(); state.width = in.i32();
    const int depth = in.u16(), mode = in.u16();
    require(depth == 8 && (mode == 3 || mode == 1), "PSD import supports 8-bit RGB and grayscale.");
    require(channels >= (mode == 3 ? 3 : 1) && channels <= 56 && state.width > 0 && state.height > 0 &&
        state.width <= maxEdge && state.height <= maxEdge &&
        std::uint64_t(state.width) * state.height <= maxDecodedPixels, "PSD dimensions exceed the decoding limit.");
    in.skip(in.u32());
    state.ppi = readResolution(in);
    const auto layerEnd = section(in, in.u32(), in.limit());
    bool hasAlpha = false;
    std::vector<Record> records;
    bool supported = true;
    if (layerEnd > in.pos()) {
        const auto infoEnd = section(in, in.u32(), layerEnd);
        if (infoEnd > in.pos()) {
            int count = signed16(in.u16());
            hasAlpha = count < 0;
            count = std::abs(count);
            supported = count <= maxLayers;
            std::uint64_t pixels = 0;
            if (supported) {
                for (int i = 0; i < count; ++i) {
                    auto rec = readRecord(in, infoEnd);
                    pixels += std::uint64_t(rec.bounds.w) * rec.bounds.h;
                    supported &= !rec.complex && rec.bounds.w <= maxEdge && rec.bounds.h <= maxEdge &&
                        pixels <= maxDecodedPixels;
                    records.push_back(std::move(rec));
                }
                for (auto& rec : records) for (auto& channel : rec.channels) {
                    channel.offset = in.pos();
                    auto end = section(in, channel.size, infoEnd);
                    if (channel.size >= 2 && in.u16() > 1 && channel.id >= -1 && channel.id <= 2) supported = false;
                    in.seek(end);
                }
            }
        }
    }
    if (supported && !records.empty()) {
        state.layers.reserve(records.size());
        for (auto i = records.rbegin(); i != records.rend(); ++i) {
            loadLayer(in, *i, mode == 1);
            state.layers.push_back(std::move(i->layer));
        }
        state.active = int(state.layers.size()) - 1;
        return state;
    }
    in.seek(layerEnd);
    return merged(in, std::move(state), channels, mode == 1, hasAlpha);
}
void savePsd(const State& state, const std::filesystem::path& path) {
    std::vector<Rect> bounds;
    std::uint64_t layerInfo = 2;
    for (auto i = state.layers.rbegin(); i != state.layers.rend(); ++i) {
        auto extent = i->pixels.extent();
        extent.x += i->x; extent.y += i->y;
        auto area = intersect(extent, state.bounds());
        if (area.empty()) area = {};
        bounds.push_back(area);
        layerInfo += 58 + extraSize(*i) + 4 * (2 + std::uint64_t(area.w) * area.h);
    }
    const auto rounded = (layerInfo + 1) & ~1ull;
    require(rounded + 78 + 4ull * state.width * state.height <= psdLimit, "This export would exceed the 2 GB PSD limit.");
    Writer out(path);
    out.str("8BPS"); out.u16(1);
    for (int i = 0; i < 6; ++i) out.u8(0);
    out.u16(4); out.u32(state.height); out.u32(state.width); out.u16(8); out.u16(3);
    out.u32(0); out.u32(28);
    out.str("8BIM"); out.u16(1005); out.u16(0); out.u32(16);
    const auto resolution = std::uint32_t(std::round(state.ppi * 65536));
    for (int axis = 0; axis < 2; ++axis) { out.u32(resolution); out.u16(1); out.u16(1); }
    out.u32(std::uint32_t(rounded + 8)); out.u32(std::uint32_t(rounded));
    out.u16(std::uint16_t(-int(state.layers.size())));
    for (std::size_t i = 0; i < bounds.size(); ++i)
        writeRecord(out, state.layers[state.layers.size() - 1 - i], bounds[i]);
    std::vector<std::uint8_t> row(state.width);
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const auto& layer = state.layers[state.layers.size() - 1 - i];
        const auto area = bounds[i];
        for (auto get : {alpha, red, green, blue}) {
            out.u16(0);
            for (int y = 0; y < area.h; ++y) {
                const int ly = y + area.y - layer.y;
                int previous = -1'000'000;
                const auto ty = tileIndex(ly);
                const Tile* tile = nullptr;
                const Tile* mask = nullptr;
                for (int x = 0; x < area.w; ++x) {
                    const int lx = x + area.x - layer.x;
                    const auto tx = tileIndex(lx);
                    if (previous != tx) {
                        previous = tx;
                        tile = layer.pixels.tile({tx, ty});
                        mask = layer.mask ? layer.mask->tile({tx, ty}) : nullptr;
                    }
                    const int offset = (ly - ty * tileEdge) * tileEdge + lx - tx * tileEdge;
                    const auto p = tile ? tile->px[offset] : 0;
                    const int value = get(p);
                    row[x] = std::uint8_t(mask && get == alpha ? (value * red(mask->px[offset]) + 127) / 255 : value);
                }
                out.bytes(row.data(), area.w);
            }
        }
    }
    if (layerInfo % 2) out.u8(0);
    out.u32(0);
    out.u16(0);
    constexpr int lines = 32;
    std::vector<Pixel> buffer(std::size_t(state.width) * lines);
    std::vector<std::uint8_t> plane(buffer.size());
    for (auto get : {red, green, blue, alpha}) {
        for (int y = 0; y < state.height; y += lines) {
            const auto h = std::min(lines, state.height - y);
            composite(state, {0, y, state.width, h}, buffer, state.width);
            for (int i = 0; i < h * state.width; ++i) plane[i] = std::uint8_t(get(straight(buffer[i])));
            out.bytes(plane.data(), std::size_t(h) * state.width);
        }
    }
    out.close();
}
} // namespace pf
