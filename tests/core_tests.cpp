#include "document.hpp"
#include "formats.hpp"
#include "binary.hpp"
#include "platform.hpp"

#include <objbase.h>
#include <psapi.h>
#include <chrono>
#include <iostream>

namespace {
using namespace pf;

void expect(bool condition, const char* message) { require(condition, message); }

Pixel sample(const State& s, int x, int y) {
    std::array<Pixel, 1> out{};
    composite(s, {x, y, 1, 1}, out, 1);
    return out[0];
}

void documentTests() {
    const auto orange = rgba(234, 121, 61);
    Document doc({12000, 8000, {Layer{}}}, L"Test", 1 << 20);
    expect(doc.pixelBytes() == 0, "Blank canvas allocated pixel data");
    doc.begin();
    const Brush brush{orange, 17, 1, 1};
    stamp(doc.active(), {0, 0, 256, 256}, 128, 128, brush);
    expect(doc.commit(L"Brush"), "Brush stroke was not committed");
    expect(doc.pixelBytes() == 4 * sizeof(Tile), "Brush used too many tiles");
    expect(sample(doc.state(), 127, 127) == orange, "Brush failed at a tile boundary");
    doc.markSaved();
    doc.begin();
    doc.edit().layers.push_back(doc.active());
    doc.select(1);
    doc.active().x = -9;
    doc.active().y = -6;
    expect(doc.commit(L"Duplicate"), "Duplicate was not committed");
    expect(doc.pixelBytes() == 4 * sizeof(Tile), "Duplicate did not share tiles");
    doc.begin();
    doc.active().mask.emplace(0xffffffff);
    fill(doc.active(), {118, 120, 3, 3}, rgba(0, 0, 0), false, true);
    doc.commit(L"Mask");
    expect(sample(doc.state(), 119, 121) != orange, "Layer mask was ignored");
    expect(doc.undo() && doc.undo() && !doc.dirty(), "Undo did not restore the saved revision");
    expect(doc.redo() && doc.dirty(), "Redo did not restore the changed revision");
    doc.begin();
    fill(doc.active(), {20, 20, 10, 10}, orange);
    doc.cancel();
    expect(sample(doc.state(), 23, 23) == 0, "Canceled edit changed pixels");
    for (int i = 0; i < 120; ++i) {
        doc.begin();
        fill(doc.active(), {i * 53 + 500, 510, 4, 4}, orange);
        doc.commit(L"Fill");
    }
    expect(doc.historyBytes() <= (1 << 20) && doc.historyCount() < 80, "History exceeded its budget");
    expect(!doc.redo(), "An edit did not discard redo entries");

    State tiny{31, 28, {Layer{}}};
    fill(tiny.layers[0], {3, 4, 2, 1}, orange);
    flip(tiny, true);
    expect(sample(tiny, 27, 4) == orange && sample(tiny, 3, 4) == 0, "Horizontal flip failed");
    flip(tiny, false);
    expect(sample(tiny, 27, 23) == orange, "Vertical flip failed");
    crop(tiny, {25, 20, 6, 6});
    expect(tiny.width == 6 && sample(tiny, 2, 3) == orange, "Crop failed to translate content");
    auto twice = resize(tiny, 12, 12);
    expect(twice.width == 12 && alpha(sample(twice, 5, 6)) > 120, "Resize lost content");
    rotateClockwise(tiny);
    expect(sample(tiny, 2, 2) == orange, "Canvas rotation failed");
    boxBlur(tiny.layers[0], tiny.bounds(), 1);
    expect(alpha(sample(tiny, 1, 2)) > 0 && alpha(sample(tiny, 0, 0)) == 0, "Box blur failed");

    Layer bucket;
    fill(bucket, {16, 0, 1, 32}, orange);
    flood(bucket, {0, 0, 32, 32}, {0, 0}, rgba(1, 2, 3), 0);
    expect(bucket.pixels.pixel(15, 25) == rgba(1, 2, 3) && bucket.pixels.pixel(17, 25) == 0, "Flood escaped its boundary");
    expect(over(0xff000000, orange, 255, Blend::multiply) == 0xff000000, "Multiply blend against black failed");
}

struct Sandbox {
    std::filesystem::path root = std::filesystem::current_path() /
        (L"pixelforge-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::vector<std::filesystem::path> files;
    Sandbox() { std::filesystem::create_directory(root); }
    ~Sandbox() {
        std::error_code ec;
        for (const auto& path : files) std::filesystem::remove(path, ec);
        std::filesystem::remove(root, ec);
    }
    std::filesystem::path file(const wchar_t* name) { return files.emplace_back(root / name); }
};

void formatTests() {
    Sandbox temp;
    State state{153, 89, {Layer{}, Layer{}}, 1};
    state.layers[0].name = L"Небо";
    state.layers[0].locked = true;
    fill(state.layers[0], {3, 8, 145, 75}, rgba(240, 128, 88));
    auto& layer = state.layers[1];
    layer.name = L"Маска α";
    layer.x = -9; layer.y = 12; layer.opacity = 216; layer.blend = Blend::screen;
    layer.mask.emplace(0xffffffff);
    fill(layer, {22, 18, 56, 41}, rgba(44, 117, 216, 194));
    fill(layer, {31, 20, 15, 16}, rgba(84, 84, 84), false, true);
    const auto pxf = temp.file(L"проект.pxf");
    save(state, pxf);
    save(state, pxf);
    const auto loaded = open(pxf);
    expect(loaded.layers.size() == 2 && loaded.active == 1 && loaded.layers[0].locked &&
        loaded.layers[1].name == layer.name && loaded.layers[1].mask && loaded.layers[1].x == -9 &&
        loaded.layers[1].blend == Blend::screen, "Project did not preserve editable state");
    expect(sample(state, 35, 25) == sample(loaded, 35, 25), "Project changed pixel values");
    auto duplicated = state;
    duplicated.layers.push_back(duplicated.layers[0]);
    save(duplicated, pxf);
    Document shared(open(pxf));
    expect(shared.pixelBytes() == Document(state).pixelBytes(), "Project did not restore shared pixel data");

    const auto psd = temp.file(L"layers.psd");
    exportImage(state, psd);
    const auto psdDoc = open(psd);
    expect(psdDoc.layers.size() == 2 && psdDoc.layers[1].name == layer.name && psdDoc.layers[1].opacity == 216,
        "PSD did not preserve layers");
    expect(sample(state, 50, 30) == sample(psdDoc, 50, 30), "PSD opaque blend did not round trip");
    const auto original = sample(state, 35, 25), restored = sample(psdDoc, 35, 25);
    expect(std::abs(red(original) - red(restored)) <= 2 && std::abs(blue(original) - blue(restored)) <= 2,
        "PSD export did not apply the layer mask");

    for (const auto name : {L"image.png", L"image.jpg", L"image.bmp", L"image.tif"}) {
        const auto path = temp.file(name);
        exportImage(state, path);
        const auto image = open(path);
        expect(image.width == state.width && image.height == state.height && image.layers.size() == 1,
            "Image dimensions or layers did not round trip");
        const auto c = sample(image, 140, 72);
        expect(std::abs(red(c) - 240) <= 4 && std::abs(green(c) - 128) <= 4 && std::abs(blue(c) - 88) <= 4,
            "Raster export changed opaque colors");
        if (std::filesystem::path(name).extension() == L".png")
            expect(alpha(sample(image, 0, 0)) == 0, "PNG export did not preserve transparency");
    }

    const auto rle = temp.file(L"packbits.psd");
    {
        Writer w(rle);
        w.str("8BPS"); w.u16(1); w.u32(0); w.u16(0);
        w.u16(3); w.u32(2); w.u32(4); w.u16(8); w.u16(3);
        w.u32(0); w.u32(0); w.u32(0);
        w.u16(1);
        for (int i = 0; i < 6; ++i) w.u16(2);
        for (int value : {15, 25, 35, 45, 55, 65}) { w.u8(253); w.u8(std::uint8_t(value)); }
        w.close();
    }
    const auto packed = open(rle);
    expect(sample(packed, 3, 1) == rgba(25, 45, 65), "PSD PackBits decoding failed");
    const auto corrupt = temp.file(L"bad.psd");
    { Writer w(corrupt); w.str("8BPS"); w.u16(1); w.close(); }
    bool rejected = false;
    try { open(corrupt); } catch (const std::runtime_error&) { rejected = true; }
    expect(rejected, "Truncated PSD was accepted");
}

void benchmark() {
    using clock = std::chrono::steady_clock;
    auto millis = [](auto start) {
        return std::chrono::duration<double, std::milli>(clock::now() - start).count();
    };
    Document sparse({12000, 8000, {Layer{}}});
    const auto sparseStart = clock::now();
    sparse.begin();
    stamp(sparse.active(), sparse.state().bounds(), 512.5f, 512.5f, Brush{rgba(255, 45, 55), 48, 0.8f, 1});
    sparse.commit(L"Brush");
    std::cout << "12000 x 8000 sparse canvas, one dab: " << sparse.pixelBytes() / 1024
        << " KiB of pixel tiles, " << millis(sparseStart) << " ms\n";

    Document photo({6000, 4000, {Layer{}}});
    auto start = clock::now();
    photo.begin();
    gradient(photo.active(), photo.state().bounds(), {0, 0}, {6000, 4000}, rgba(20, 111, 170), rgba(240, 145, 67));
    photo.commit(L"Gradient");
    std::cout << "6000 x 4000 dense gradient: " << photo.pixelBytes() / 1048576.0 << " MiB of pixel tiles, "
        << millis(start) << " ms\n";
    std::vector<Pixel> pixels(tileEdge * tileEdge);
    start = clock::now();
    for (int i = 0; i < 500; ++i)
        preview(photo.state(), {(i % 8) * 512, (i % 6) * 512, 512, 512}, 4, tileEdge, tileEdge, pixels);
    std::cout << "500 downsampled 128 x 128 preview tiles: " << millis(start) << " ms\n";
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
        std::cout << "Process peak working set: " << info.PeakWorkingSetSize / 1048576.0 << " MiB\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    try {
        documentTests();
        formatTests();
        std::cout << "PixelForge core tests passed.\n";
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmark();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return 0;
}
