#include "formats.hpp"
#include "binary.hpp"
#include "platform.hpp"

#include <wincodec.h>
#include <cwctype>

namespace pf {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view signature = "PXFORGE2";
constexpr std::size_t projectTileLimit = (1024ull << 20) / sizeof(Tile);
constexpr std::size_t projectReferenceLimit = 1'000'000;

void check(HRESULT hr, const char* message) { require(SUCCEEDED(hr), message); }

Com<IWICImagingFactory> factory() {
    Com<IWICImagingFactory> result;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory, reinterpret_cast<void**>(result.put())), "Windows Imaging Component is unavailable.");
    return result;
}
std::wstring extension(const fs::path& path) {
    auto e = path.extension().wstring();
    std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return e;
}
std::string utf8(const std::wstring& value) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(length, '\0');
    if (length) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), int(value.size()), text.data(), length, nullptr, nullptr);
    return text;
}
std::wstring fromUtf8(const std::string& value) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    require(length > 0 || value.empty(), "Invalid layer name.");
    std::wstring text(length, L'\0');
    if (length) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), text.data(), length);
    return text;
}

template<class F>
void atomicWrite(const fs::path& destination, F write) {
    GUID token;
    check(CoCreateGuid(&token), "Unable to reserve a temporary file.");
    wchar_t id[40]{};
    StringFromGUID2(token, id, 40);
    const auto temp = destination.parent_path() / (std::wstring(L".pixelforge-") + id + L".tmp");
    try {
        write(temp);
        if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
            throw std::system_error(int(GetLastError()), std::system_category(), "Unable to replace the destination file");
    } catch (...) {
        DeleteFileW(temp.c_str());
        throw;
    }
}

void writePlane(Writer& out, const Plane& plane, std::unordered_map<const Tile*, std::uint32_t>& dictionary,
    std::size_t& remainingReferences) {
    require(plane.tiles().size() <= remainingReferences, "Project exceeds the tile reference limit.");
    remainingReferences -= plane.tiles().size();
    out.u32(std::uint32_t(plane.tiles().size()));
    std::array<std::uint16_t, tileEdge * tileEdge> lengths{};
    for (const auto& [coord, tile] : plane.tiles()) {
        out.u32(std::uint32_t(coord.x));
        out.u32(std::uint32_t(coord.y));
        const auto [entry, inserted] = dictionary.try_emplace(tile.get(), std::uint32_t(dictionary.size()));
        out.u32(entry->second);
        if (!inserted) continue;
        require(dictionary.size() <= projectTileLimit, "Project exceeds the 1 GB pixel data limit.");
        int runs = 0;
        for (std::size_t i = 0; i < tile->px.size();) {
            std::size_t j = i + 1;
            while (j < tile->px.size() && tile->px[j] == tile->px[i]) ++j;
            lengths[runs++] = std::uint16_t(j - i);
            i = j;
        }
        if (runs * 6 >= int(sizeof(Tile))) {
            out.u16(0);
            out.bytes(tile->px.data(), sizeof(Tile)); // Native project format uses little-endian BGRA tiles.
        } else {
            out.u16(std::uint16_t(runs));
            std::size_t i = 0;
            for (int r = 0; r < runs; ++r) {
                out.u16(lengths[r]);
                out.u32(tile->px[i]);
                i += lengths[r];
            }
        }
    }
}
Plane readPlane(Reader& in, Pixel background, std::vector<std::shared_ptr<Tile>>& dictionary,
    std::size_t& remainingReferences, bool shared) {
    const auto count = in.u32();
    require(count <= remainingReferences && count <= (in.limit() - in.pos()) / (shared ? 12 : 16),
        "Project tile data exceeds the decoding limit.");
    remainingReferences -= count;
    Plane plane(background);
    for (std::uint32_t t = 0; t < count; ++t) {
        const Coord coord{in.i32(), in.i32()};
        require(coord.x >= -8192 && coord.x <= 8192 && coord.y >= -8192 && coord.y <= 8192 &&
            !plane.tile(coord), "Invalid tile coordinates.");
        if (shared) {
            const auto id = in.u32();
            require(id <= dictionary.size(), "Invalid project tile reference.");
            if (id < dictionary.size()) {
                plane.assign(coord, dictionary[id]);
                continue;
            }
        }
        require(dictionary.size() < projectTileLimit, "Project exceeds the 1 GB pixel data limit.");
        const auto runs = in.u16();
        auto tile = std::make_shared<Tile>();
        if (!runs) {
            in.bytes(tile->px.data(), sizeof(Tile));
        } else {
            std::size_t i = 0;
            for (int r = 0; r < runs; ++r) {
                const auto n = in.u16();
                const auto v = in.u32();
                require(n && i + n <= tile->px.size(), "Invalid tile run.");
                std::fill_n(tile->px.begin() + i, n, v);
                i += n;
            }
            require(i == tile->px.size(), "Incomplete tile.");
        }
        dictionary.push_back(tile);
        plane.assign(coord, std::move(tile));
    }
    return plane;
}
State readProject(const fs::path& path) {
    Reader in(path);
    const auto magic = in.str(signature.size());
    require(magic == signature || magic == "PXFORGE1", "Invalid PixelForge project.");
    State state;
    state.width = in.i32();
    state.height = in.i32();
    const auto count = in.u16();
    state.active = in.u16();
    require(state.width > 0 && state.height > 0 && state.width <= maxEdge && state.height <= maxEdge &&
        count > 0 && count <= maxLayers && state.active < count, "Invalid project dimensions or layer count.");
    std::size_t remainingReferences = projectReferenceLimit;
    std::vector<std::shared_ptr<Tile>> dictionary;
    state.layers.reserve(count);
    for (int i = 0; i < count; ++i) {
        Layer layer;
        layer.name = fromUtf8(in.str(in.u16()));
        layer.x = in.i32(); layer.y = in.i32();
        layer.opacity = in.u8();
        layer.blend = Blend(in.u8());
        const auto flags = in.u8();
        require(layer.x >= -maxEdge * 64 && layer.x <= maxEdge * 64 && layer.y >= -maxEdge * 64 &&
            layer.y <= maxEdge * 64 && layer.blend <= Blend::difference && flags <= 7, "Invalid layer metadata.");
        layer.visible = (flags & 1) != 0;
        layer.locked = (flags & 2) != 0;
        layer.pixels = readPlane(in, 0, dictionary, remainingReferences, magic == signature);
        if (flags & 4) layer.mask = readPlane(in, 0xffffffff, dictionary, remainingReferences, magic == signature);
        state.layers.push_back(std::move(layer));
    }
    return state;
}

State decode(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, std::wstring name) {
    Com<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, frame.put()), "Unable to decode the first image.");
    UINT width{}, height{};
    check(frame->GetSize(&width, &height), "Unable to read image dimensions.");
    require(width && height && width <= maxEdge && height <= maxEdge && std::uint64_t(width) * height <= maxDecodedPixels,
        "The image exceeds the 16,384 px edge or 128 megapixel import limit.");
    Com<IWICFormatConverter> converter;
    check(wic->CreateFormatConverter(converter.put()), "Unable to create a pixel converter.");
    check(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
        0, WICBitmapPaletteTypeCustom), "This pixel format is not supported.");
    State state{int(width), int(height), {Layer{}}};
    auto& layer = state.layers.front();
    layer.name = std::move(name);
    constexpr int rows = 24;
    std::vector<Pixel> band(std::size_t(width) * rows);
    for (int y = 0; y < int(height); y += rows) {
        const int h = std::min(rows, int(height) - y);
        WICRect roi{0, y, int(width), h};
        check(converter->CopyPixels(&roi, width * 4, width * h * 4, reinterpret_cast<BYTE*>(band.data())),
            "Unable to read image pixels.");
        importPixels(layer.pixels, {0, y, int(width), h}, band, int(width));
    }
    return state;
}

void encode(const State& state, const fs::path& path, const GUID& codec) {
    auto wic = factory();
    Com<IWICStream> stream;
    check(wic->CreateStream(stream.put()), "Unable to create an image stream.");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Unable to open the export destination.");
    Com<IWICBitmapEncoder> encoder;
    check(wic->CreateEncoder(codec, nullptr, encoder.put()), "Image encoder unavailable.");
    check(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache), "Unable to initialize the image encoder.");
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> properties;
    check(encoder->CreateNewFrame(frame.put(), properties.put()), "Unable to create an image frame.");
    if (codec == GUID_ContainerFormatJpeg && properties) {
        wchar_t key[] = L"ImageQuality";
        PROPBAG2 property{};
        property.pstrName = key;
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = 0.94f;
        properties->Write(1, &property, &value);
    }
    check(frame->Initialize(properties.get()), "Unable to initialize the image frame.");
    check(frame->SetSize(state.width, state.height), "The encoder does not support this image size.");
    WICPixelFormatGUID format = codec == GUID_ContainerFormatPng || codec == GUID_ContainerFormatTiff
        ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&format), "Unable to select an export pixel format.");
    const bool alpha = format == GUID_WICPixelFormat32bppBGRA || format == GUID_WICPixelFormat32bppPBGRA;
    require(alpha || format == GUID_WICPixelFormat24bppBGR, "The encoder returned an unsupported pixel format.");
    constexpr int rows = 32;
    const UINT stride = state.width * (alpha ? 4 : 3);
    std::vector<Pixel> band(std::size_t(state.width) * rows);
    std::vector<BYTE> rgb(alpha ? 0 : std::size_t(stride) * rows);
    for (int y = 0; y < state.height; y += rows) {
        int h = std::min(rows, state.height - y);
        composite(state, {0, y, state.width, h}, band, state.width);
        const int count = h * state.width;
        BYTE* data = reinterpret_cast<BYTE*>(band.data());
        if (format == GUID_WICPixelFormat32bppBGRA) {
            for (int i = 0; i < count; ++i) band[i] = straight(band[i]);
        } else if (!alpha) {
            for (int i = 0; i < count; ++i) {
                const int white = 255 - pf::alpha(band[i]);
                rgb[i * 3] = BYTE(blue(band[i]) + white);
                rgb[i * 3 + 1] = BYTE(green(band[i]) + white);
                rgb[i * 3 + 2] = BYTE(red(band[i]) + white);
            }
            data = rgb.data();
        }
        check(frame->WritePixels(h, stride, h * stride, data), "Unable to encode image pixels.");
    }
    check(frame->Commit(), "Unable to finish the image frame.");
    check(encoder->Commit(), "Unable to finish the image file.");
}
} // namespace

State open(const fs::path& path) {
    const auto e = extension(path);
    if (e == L".pxf") return readProject(path);
    if (e == L".psd") return loadPsd(path);
    auto wic = factory();
    Com<IWICBitmapDecoder> decoder;
    check(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()),
        "Windows could not open this image format.");
    return decode(wic.get(), decoder.get(), path.stem().wstring());
}
State demo() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(101), RT_RCDATA);
    require(resource != nullptr, "Sample image is unavailable.");
    auto data = static_cast<BYTE*>(LockResource(LoadResource(instance, resource)));
    const auto size = SizeofResource(instance, resource);
    auto wic = factory();
    Com<IWICStream> stream;
    check(wic->CreateStream(stream.put()), "Unable to create the sample image stream.");
    check(stream->InitializeFromMemory(data, size), "Unable to open the sample image.");
    Com<IWICBitmapDecoder> decoder;
    check(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()),
        "Unable to decode the sample image.");
    return decode(wic.get(), decoder.get(), L"Coastal Road");
}
void save(const State& state, const fs::path& path) {
    atomicWrite(path, [&](const fs::path& temp) {
        Writer out(temp);
        out.str(signature);
        out.u32(state.width); out.u32(state.height);
        out.u16(std::uint16_t(state.layers.size())); out.u16(std::uint16_t(state.active));
        std::unordered_map<const Tile*, std::uint32_t> dictionary;
        std::size_t remainingReferences = projectReferenceLimit;
        for (const auto& layer : state.layers) {
            const auto name = utf8(layer.name);
            require(name.size() <= 65535, "A layer name is too long.");
            require(layer.x >= -maxEdge * 64 && layer.x <= maxEdge * 64 && layer.y >= -maxEdge * 64 &&
                layer.y <= maxEdge * 64, "Move the layer closer to the canvas before saving.");
            out.u16(std::uint16_t(name.size())); out.str(name);
            out.u32(layer.x); out.u32(layer.y); out.u8(std::uint8_t(layer.opacity));
            out.u8(std::uint8_t(layer.blend));
            out.u8(std::uint8_t((layer.visible ? 1 : 0) | (layer.locked ? 2 : 0) | (layer.mask ? 4 : 0)));
            writePlane(out, layer.pixels, dictionary, remainingReferences);
            if (layer.mask) writePlane(out, *layer.mask, dictionary, remainingReferences);
        }
        out.close();
    });
}
void exportImage(const State& state, const fs::path& path) {
    const auto e = extension(path);
    if (e == L".psd") {
        atomicWrite(path, [&](const fs::path& temp) { savePsd(state, temp); });
        return;
    }
    const auto& codec = e == L".png" ? GUID_ContainerFormatPng
        : e == L".jpg" || e == L".jpeg" ? GUID_ContainerFormatJpeg
        : e == L".tif" || e == L".tiff" ? GUID_ContainerFormatTiff : GUID_ContainerFormatBmp;
    require(e == L".png" || e == L".jpg" || e == L".jpeg" || e == L".bmp" || e == L".tif" || e == L".tiff",
        "Use a PNG, JPG, TIFF, BMP or PSD filename.");
    atomicWrite(path, [&](const fs::path& temp) { encode(state, temp, codec); });
}
} // namespace pf
