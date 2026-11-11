#include "app.hpp"
#include "binary.hpp"

namespace pf {
namespace {
class Clipboard {
public:
    explicit Clipboard(HWND window) { require(OpenClipboard(window), "The clipboard is busy."); }
    ~Clipboard() { CloseClipboard(); }
};
class GlobalData {
public:
    explicit GlobalData(HGLOBAL data, bool owns = false) : data_(data), owns_(owns), address_(GlobalLock(data)) {}
    ~GlobalData() { unlock(); if (owns_) GlobalFree(data_); }
    void* address() const { return address_; }
    void unlock() { if (std::exchange(address_, nullptr)) GlobalUnlock(data_); }
    void publish(UINT type) {
        unlock();
        require(SetClipboardData(type, data_) != nullptr, "Unable to publish the clipboard image.");
        owns_ = false;
    }
private:
    HGLOBAL data_;
    bool owns_;
    void* address_;
};
} // namespace

void App::copy(bool cut, bool merged) {
    auto t = tab();
    auto& doc = t->doc;
    const auto area = clip();
    if (area.empty()) return;
    const auto count = std::size_t(area.w) * area.h;
    require(count <= maxDecodedPixels, "The clipboard image is too large. Select a smaller area.");
    constexpr auto headerBytes = sizeof(BITMAPV5HEADER);
    auto data = GlobalAlloc(GMEM_MOVEABLE, headerBytes + count * sizeof(Pixel));
    require(data != nullptr, "Unable to allocate the clipboard image.");
    GlobalData object(data, true);
    require(object.address() != nullptr, "Unable to lock the clipboard image.");
    auto* header = static_cast<BITMAPV5HEADER*>(object.address());
    *header = {};
    header->bV5Size = headerBytes; header->bV5Width = area.w; header->bV5Height = -area.h;
    header->bV5Planes = 1; header->bV5BitCount = 32; header->bV5Compression = BI_BITFIELDS;
    header->bV5SizeImage = DWORD(count * sizeof(Pixel));
    header->bV5RedMask = 0x00ff0000; header->bV5GreenMask = 0x0000ff00;
    header->bV5BlueMask = 0x000000ff; header->bV5AlphaMask = 0xff000000;
    header->bV5CSType = 0x73524742; // LCS_sRGB
    auto pixels = std::span(reinterpret_cast<Pixel*>(header + 1), count);
    if (!merged) {
        const auto& layer = doc.active();
        for (int y = 0; y < area.h; ++y) for (int x = 0; x < area.w; ++x) {
            const int xx = x + area.x - layer.x, yy = y + area.y - layer.y;
            pixels[std::size_t(y) * area.w + x] = t->mask ? layer.mask->pixel(xx, yy) : layer.pixels.pixel(xx, yy);
        }
    } else {
        composite(doc.state(), area, pixels, area.w);
        for (auto& p : pixels) p = straight(p);
    }
    {
        Clipboard clipboard(window_);
        require(EmptyClipboard(), "Unable to empty the clipboard.");
        object.publish(CF_DIBV5);
    }
    if (cut) {
        doc.begin();
        fill(doc.active(), area, 0xffffffff, true, t->mask);
        doc.commit(L"Cut");
        documentChanged();
    }
}
void App::paste() {
    Layer layer;
    layer.name = L"Pasted Layer";
    int w{}, h{};
    {
        Clipboard clipboard(window_);
        auto handle = GetClipboardData(CF_DIBV5);
        if (!handle) handle = GetClipboardData(CF_DIB);
        if (!handle) return;
        const auto size = GlobalSize(handle);
        GlobalData data(handle);
        require(data.address() && size >= sizeof(BITMAPINFOHEADER), "Invalid clipboard image.");
        const auto header = static_cast<const BITMAPINFOHEADER*>(data.address());
        require(header->biSize >= sizeof(BITMAPINFOHEADER) && header->biSize <= size &&
            header->biWidth > 0 && header->biWidth <= maxEdge && header->biHeight &&
            header->biHeight >= -maxEdge && header->biHeight <= maxEdge && header->biPlanes == 1 &&
            (header->biBitCount == 24 || header->biBitCount == 32) &&
            (header->biCompression == BI_RGB || (header->biCompression == BI_BITFIELDS && header->biBitCount == 32)),
            "This clipboard image format is unsupported.");
        w = header->biWidth; h = std::abs(header->biHeight);
        const int channels = header->biBitCount / 8;
        const int pitch = (w * channels + 3) & ~3;
        const std::size_t base = header->biSize + (header->biCompression == BI_BITFIELDS && header->biSize == 40 ? 12 : 0);
        require(std::uint64_t(w) * h <= maxDecodedPixels && base <= size && std::size_t(h) * pitch <= size - base,
            "Clipboard image data is incomplete or too large.");
        bool hasAlpha = false;
        if (header->biCompression == BI_BITFIELDS) {
            const auto masks = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(data.address()) + 40);
            require(base >= 52 && masks[0] == 0xff0000 && masks[1] == 0xff00 && masks[2] == 0xff,
                "Clipboard channel masks are unsupported.");
            hasAlpha = header->biSize >= 56 && masks[3] == 0xff000000;
        }
        const auto bytes = static_cast<const BYTE*>(data.address()) + base;
        constexpr int batch = 24;
        std::vector<Pixel> buffer(std::size_t(w) * batch);
        for (int y = 0; y < h; y += batch) {
            const int lines = std::min(batch, h - y);
            for (int j = 0; j < lines; ++j) {
                const auto row = bytes + std::size_t(header->biHeight < 0 ? y + j : h - 1 - y - j) * pitch;
                for (int x = 0; x < w; ++x) {
                    const auto bgr = row + x * channels;
                    buffer[std::size_t(j) * w + x] = rgba(bgr[2], bgr[1], bgr[0], hasAlpha ? bgr[3] : 255);
                }
            }
            importPixels(layer.pixels, {0, y, w, lines}, buffer, w);
        }
    }
    if (auto t = tab()) {
        auto& doc = t->doc;
        auto& state = doc.edit();
        require(state.layers.size() < maxLayers, "This document has reached its layer limit.");
        layer.x = (state.width - w) / 2;
        layer.y = (state.height - h) / 2;
        doc.begin();
        state.layers.insert(state.layers.begin() + ++state.active, std::move(layer));
        doc.commit(L"Paste");
        t->mask = false;
        documentChanged();
    } else {
        addTab({w, h, {std::move(layer)}}, L"Clipboard", L"Paste");
    }
}
} // namespace pf
