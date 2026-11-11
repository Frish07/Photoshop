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

void editingTests() {
    const auto white = rgba(255, 255, 255), orange = rgba(230, 96, 43);
    Document large(blankDocument(12000, 8000, 300, white), L"New Document");
    expect(large.state().layers.size() == 2 && large.state().layers[0].locked && !large.active().locked,
        "White canvas must be ready for painting above a locked background");
    expect(sample(large.state(), 11999, 7999) == white && large.pixelBytes() <= 4 * sizeof(Tile),
        "White background used dense storage or missed the last tile");
    Document larger(resize(large.state(), 13000, 8100));
    expect(sample(larger.state(), 12999, 8099) == white && larger.pixelBytes() <= 4 * sizeof(Tile),
        "Resizing a uniform background used dense storage");
    Document transparent(blankDocument(1920, 1080, 72, 0));
    expect(transparent.pixelBytes() == 0 && sample(transparent.state(), 8, 9) == 0, "Transparent preset allocated pixels");

    Document paint({160, 120, {Layer{}, Layer{}}, 1});
    fill(paint.edit().layers[0], {12, 10, 24, 24}, orange);
    paint.begin();
    const CloneSample clone{paint.beforeEdit(), {-45, -30}, true};
    stamp(paint.active(), paint.state().bounds(), 65.5f, 50.5f, Brush{0, 9, 1, 1}, &clone);
    expect(paint.commit(L"Clone") && sample(paint.state(), 65, 50) == orange, "Merged clone stamp used the wrong source");
    expect(sample(paint.historyStart(), 65, 50) == 0, "Initial history was changed by a clone stroke");

    const Rect from{61, 46, 9, 9}, to{76, 69, 18, 18};
    const TransformView pose{from, to, 90};
    std::array<Pixel, 1> previewed{};
    preview(paint.state(), {85, 78, 1, 1}, 1, 1, 1, previewed, &pose);
    expect(previewed.front() == orange, "Transform preview missed the projected layer");
    paint.begin();
    transformLayer(paint.active(), from, to, 90);
    paint.commit(L"Transform");
    expect(sample(paint.state(), 85, 78) == orange && sample(paint.state(), 65, 50) == 0,
        "Committed transform did not match the preview");

    State tinted{80, 64, {Layer{}, Layer{}}, 1, 144};
    fill(tinted.layers[0], {12, 11, 28, 24}, rgba(255, 0, 0));
    tinted.layers[1].adjustment = Adjustment{AdjustmentKind::hueSaturation, 120, 0, 0};
    tinted.layers[1].mask = maskFromSelection(tinted.bounds(), Rect{12, 11, 14, 24});
    expect(sample(tinted, 18, 20) == rgba(0, 255, 0) && sample(tinted, 32, 20) == rgba(255, 0, 0) &&
        alpha(sample(tinted, 0, 0)) == 0, "Adjustment changed pixels outside its mask or lost transparency");
    const auto retained = tinted;
    mergeVisible(tinted, false);
    expect(tinted.layers.size() == 1 && sample(tinted, 18, 20) == sample(retained, 18, 20) &&
        sample(tinted, 32, 20) == sample(retained, 32, 20), "Merge Visible changed the appearance");
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
    State state{153, 89, {Layer{}, Layer{}}, 1, 144};
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
    expect(loaded.ppi == 144, "Project did not preserve resolution");
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
    expect(psdDoc.ppi == 144, "PSD did not preserve resolution");
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
        if (std::filesystem::path(name).extension() == L".png") {
            expect(alpha(sample(image, 0, 0)) == 0, "PNG export did not preserve transparency");
            expect(std::abs(image.ppi - 144) < 0.1, "PNG did not preserve resolution");
        }
    }

    Layer correction;
    correction.adjustment = Adjustment{AdjustmentKind::levels, 10, 230, 180};
    correction.mask = maskFromSelection(state.bounds(), Rect{3, 7, 32, 40});
    state.layers.push_back(std::move(correction));
    save(state, pxf);
    const auto adjusted = open(pxf);
    expect(adjusted.layers.back().adjustment == state.layers.back().adjustment &&
        sample(adjusted, 12, 18) == sample(state, 12, 18), "Editable adjustment did not survive saving");
    exportImage(state, psd);
    const auto baked = open(psd);
    expect(baked.layers.size() == 1 && sample(baked, 12, 18) == sample(state, 12, 18),
        "PSD export did not render adjustment layers");

    const auto old = temp.file(L"legacy.pxf");
    {
        Writer w(old);
        w.str("PXFORGE2"); w.u32(8); w.u32(6); w.u16(1); w.u16(0);
        w.u16(3); w.str("Old"); w.u32(0); w.u32(0); w.u8(255); w.u8(0); w.u8(1); w.u32(0);
        w.close();
    }
    expect(open(old).width == 8 && open(old).layers.front().name == L"Old", "Legacy PXF project could not be opened");

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
        editingTests();
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
