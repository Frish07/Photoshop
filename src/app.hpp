#pragma once

#include "document.hpp"
#include "platform.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <filesystem>

namespace pf {
using Box = D2D1_RECT_F;
using Vec = D2D1_POINT_2F;
inline Box box(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
inline bool inside(Box r, Vec p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
inline float width(Box r) { return r.right - r.left; }
inline float height(Box r) { return r.bottom - r.top; }

enum class Tool { move, marquee, ellipticalMarquee, lasso, polygonLasso, wand, crop, picker, brush, clone, eraser,
    bucket, gradient, type, rectangle, ellipse, hand, zoom };
enum class Icon { move, marquee, crop, picker, brush, eraser, bucket, gradient, type, rectangle, ellipse,
    hand, zoom, more, plus, close, eye, hidden, lock, mask, trash, down, up, check, fit, image, swap, layers,
    clone, compare, adjustment, transform, lasso, polygonLasso, wand };
enum class Command : int {
    none, newDoc, open, demo, save, saveAs, exportFile, close, quit,
    undo, redo, cut, copy, copyMerged, paste, clear, fillFG, fillBG,
    resize, canvasSize, flipX, flipY, rotate, invert, gray, brightness, contrast, blur,
    addLayer, place, rename, duplicate, deleteLayer, layerUp, layerDown, addMask, removeMask, toggleLock,
    addTone, addLevels, addHue, editAdjustment, mergeDown, mergeVisible, flatten, freeTransform, applyTransform, cancelTransform,
    strokeStyle, overlayStyle, shadowStyle, clearStyle,
    selectAll, deselect, inverseSelection, reselect, loadSelection, layerViaCut, cropSelection, fit, actual, zoomIn, zoomOut,
    rulers, panels, rightPanels, extras, contextBar, compare, resolution, newLayerDialog, shortcuts, about,
    guides, lockGuides, newGuide, clearGuides, snap, screenStandard, screenMenus, screenFull
};
enum class Hit { menu, tab, closeTab, tool, color, swap, defaults, fgSize, hardness, flow, typeSize, tolerance,
    fit, undo, redo, blend, opacity, layer, eye, lock, mask, history, panel, colorTab, saturation, hue,
    swatch, command, zoom, applyCrop, cancelCrop, moreTools, zoomMode, cloneSample, compareSlider,
    transformSize, transformAngle, transformLink, properties, selectionMode, wandContiguous, wandMerged, layerThumbnail,
    fillOpacity, styles, ruler, autoSelect };
struct Target { Box rect; Hit kind; int id{}; std::wstring tip; };

struct Transform {
    Rect original, box;
    double degrees{};
    bool linked = true;
};

struct Tab {
    Tab(State state, std::wstring title, std::wstring history);
    Document doc;
    std::wstring title;
    std::filesystem::path path;
    float zoom = 1;
    Vec pan{};
    bool fit = true;
    bool mask = false;
    bool compare = false;
    float split = 0.5f;
    int layerScroll{};
    int historyScroll{};
    std::optional<Rect> cropBox;
    std::optional<Selection> lastSelection;
    std::optional<Point> cloneSource;
    std::optional<Transform> transform;
};
struct ToolInfo { Tool tool; Icon icon; wchar_t key; const wchar_t* name; };
inline constexpr ToolInfo tools[] = {
    {Tool::move, Icon::move, L'V', L"Move"}, {Tool::marquee, Icon::marquee, L'M', L"Rectangular Marquee"},
    {Tool::ellipticalMarquee, Icon::ellipse, L'M', L"Elliptical Marquee"},
    {Tool::lasso, Icon::lasso, L'L', L"Lasso"}, {Tool::polygonLasso, Icon::polygonLasso, L'L', L"Polygonal Lasso"},
    {Tool::wand, Icon::wand, L'W', L"Magic Wand"},
    {Tool::crop, Icon::crop, L'C', L"Crop"}, {Tool::picker, Icon::picker, L'I', L"Eyedropper"},
    {Tool::brush, Icon::brush, L'B', L"Brush"}, {Tool::clone, Icon::clone, L'S', L"Clone Stamp"},
    {Tool::eraser, Icon::eraser, L'E', L"Eraser"},
    {Tool::gradient, Icon::gradient, L'G', L"Gradient"}, {Tool::bucket, Icon::bucket, L'G', L"Paint Bucket"},
    {Tool::type, Icon::type, L'T', L"Horizontal Type"}, {Tool::rectangle, Icon::rectangle, L'U', L"Rectangle"},
    {Tool::ellipse, Icon::ellipse, L'U', L"Ellipse"}, {Tool::hand, Icon::hand, L'H', L"Hand"},
    {Tool::zoom, Icon::zoom, L'Z', L"Zoom"}
};
inline bool selectionTool(Tool t) {
    return t == Tool::marquee || t == Tool::ellipticalMarquee || t == Tool::lasso || t == Tool::polygonLasso || t == Tool::wand;
}
struct BlendOption { Blend mode; const wchar_t* name; bool separator = false; };
inline constexpr BlendOption blendOptions[] = {
    {Blend::normal, L"Normal"},
    {Blend::darken, L"Darken", true}, {Blend::multiply, L"Multiply"}, {Blend::colorBurn, L"Color Burn"},
    {Blend::linearBurn, L"Linear Burn"}, {Blend::darkerColor, L"Darker Color"},
    {Blend::lighten, L"Lighten", true}, {Blend::screen, L"Screen"}, {Blend::colorDodge, L"Color Dodge"},
    {Blend::linearDodge, L"Linear Dodge (Add)"}, {Blend::lighterColor, L"Lighter Color"},
    {Blend::overlay, L"Overlay", true}, {Blend::softLight, L"Soft Light"}, {Blend::hardLight, L"Hard Light"},
    {Blend::vividLight, L"Vivid Light"}, {Blend::linearLight, L"Linear Light"}, {Blend::pinLight, L"Pin Light"}, {Blend::hardMix, L"Hard Mix"},
    {Blend::difference, L"Difference", true}, {Blend::exclusion, L"Exclusion"}, {Blend::subtract, L"Subtract"}, {Blend::divide, L"Divide"},
    {Blend::hue, L"Hue", true}, {Blend::saturation, L"Saturation"}, {Blend::color, L"Color"}, {Blend::luminosity, L"Luminosity"}
};
inline const wchar_t* blendName(Blend mode) {
    for (const auto& option : blendOptions) if (mode == option.mode) return option.name;
    return L"Normal";
}

struct Field { std::wstring label, value; bool multiline{}; };
std::optional<std::vector<std::wstring>> prompt(HWND owner, const std::wstring& title, std::span<const Field> fields);
int readNumber(const std::wstring& text, int low, int high);
struct NewDocumentOptions {
    std::wstring name;
    int width{}, height{};
    double ppi = 72;
    Pixel background = 0xffffffff;
    bool openExisting = false;
};
std::optional<NewDocumentOptions> newDocumentDialog(HWND owner, std::wstring title, Pixel background);
std::filesystem::path fileDialog(HWND owner, bool save, bool exportFile, const std::wstring& name = L"");
void drawType(Layer& layer, Rect clip, Point where, const std::wstring& text, int size, Pixel color,
    const wchar_t* font = L"Segoe UI", int weight = FW_NORMAL);

class App {
public:
    explicit App(HINSTANCE instance);
    bool create(int show);
    void openPath(const std::filesystem::path& path, bool placed = false);
    void openDemo();
    void createDocument();
    LRESULT message(UINT msg, WPARAM w, LPARAM l);
    static LRESULT CALLBACK windowProc(HWND window, UINT msg, WPARAM w, LPARAM l);
private:
    using Color = std::uint32_t;
    struct Bitmap { Rect area; Com<ID2D1Bitmap> image; std::uint64_t touched{}; };
    struct Thumbnail {
        Plane pixels;
        std::optional<Plane> mask;
        Com<ID2D1Bitmap> image;
    };
    enum class Drag { none, paint, pan, move, marquee, lasso, crop, gradient, shape, colorSV, colorHue,
        layerRow, compare, transform, guide };
    using BitmapCache = std::unordered_map<Coord, Bitmap, CoordHash>;

    Tab* tab() { return tabs_.empty() ? nullptr : tabs_[current_].get(); }
    const Tab* tab() const { return tabs_.empty() ? nullptr : tabs_[current_].get(); }
    Rect clip() const;
    const Selection* selection() const { return tab() && tab()->doc.selection ? &*tab()->doc.selection : nullptr; }
    void setSelection(Selection selected, std::wstring label = L"Selection");
    SelectMode selectionModeAtPress() const;
    void finishLasso();
    void setScreenMode(int mode);
    std::optional<int> nearestGuide(Vec p) const;
    void startGuide(bool vertical, std::optional<int> existing, Vec p);
    void finishGuide(Vec p);
    Point snappedMove(Rect from, Point offset) const;
    Point snappedPoint(Point point) const;
    Vec mouse(LPARAM p) const;
    Vec imageToScreen(float x, float y) const;
    Vec screenToImage(Vec p) const;
    Point imagePoint(Vec p) const;
    Box imageRect() const;
    void layout();
    void repaint();
    void clearCache();
    void invalidate(Rect dirty);
    void documentChanged(bool pixels = true);
    void updateTitle();
    void addTab(State state, std::wstring title, std::wstring action);
    void switchTab(std::size_t index);
    bool closeTab(std::size_t index);
    bool saveTab(Tab& t, bool as);
    void zoomAt(float zoom, Vec at);
    void fit();
    void setTool(Tool t);
    void chooseColor(bool foreground);
    void changeHSV(float h, float s, float v);
    void updateHSV();
    void command(Command c);
    void inputTarget(const Target& target, Vec p);
    void menu(int index, Vec point, bool context = false);
    void popupBlend(Vec at);
    void popupStyles(Vec at);
    void popupTools(Vec at, wchar_t group = 0);
    void number(const wchar_t* label, int& value, int low, int high);
    void leftDown(Vec p, bool middle = false);
    void moveMouse(Vec p);
    void leftUp(Vec p);
    void cancelDrag();
    void stroke(Vec p);
    void dab(float x, float y, Rect& dirty);
    void adjustment(std::optional<AdjustmentKind> kind);
    void layerStyle(Command command);
    void beginTransform();
    void resetTransform();
    void applyTransform();
    void transformNumber(bool angle);
    std::array<Vec, 9> transformHandles() const;
    bool startTransformDrag(Vec p);
    void dragTransform(Vec p);
    bool key(WPARAM key);
    void copy(bool cut, bool merged = false);
    void paste();
    void reportError(const std::exception& error);

    bool createTarget();
    void paint();
    void drawHeader();
    void drawToolbar();
    void drawSidebar();
    void drawColorPanel(Box area);
    void drawLayers(Box area);
    void drawHistory(Box area);
    void drawProperties(Box area);
    void drawCanvas();
    void drawContextBar();
    void drawTransform();
    void drawSelection();
    void drawSelectionGesture();
    void drawStatus();
    void drawRulers();
    void drawGuides();
    void drawHome();
    void drawTooltip();
    void hit(Box r, Hit kind, int id = 0, std::wstring tip = L"");
    void rect(Box r, Color c, float alpha = 1);
    void strokeRect(Box r, Color c, float weight = 1);
    void rounded(Box r, float radius, Color c, float alpha = 1);
    void line(float x1, float y1, float x2, float y2, Color c, float weight = 1);
    void circle(float x, float y, float radius, Color c, bool fill = false, float weight = 1);
    void text(Box r, std::wstring_view text, Color color, int style = 0,
        DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    void icon(Icon icon, float x, float y, Color c);
    void button(Box r, std::wstring_view label, Hit kind, int id = 0, bool on = false, std::wstring tip = L"");
    void iconButton(Box r, Icon i, Hit kind, int id = 0, bool on = false, std::wstring tip = L"");
    void thumb(Box r, const Layer& l, bool mask);
    void chequer(Box area, float size, Color c1, Color c2);
    void drawTiles(const State& state, Box visible, int step, BitmapCache& cache);
    void cacheTile(const State& state, Rect area, int step, Coord key, BitmapCache& cache);

    HINSTANCE instance_;
    HWND window_{};
    Com<ID2D1Factory> factory_;
    Com<IDWriteFactory> write_;
    Com<ID2D1HwndRenderTarget> target_;
    Com<ID2D1SolidColorBrush> brush_;
    Com<ID2D1Bitmap> checker_, spectrum_, hueStrip_;
    Com<ID2D1BitmapBrush> checkerBrush_;
    Com<ID2D1PathGeometry> selectionPath_;
    std::array<Com<ID2D1StrokeStyle>, 4> marchingStyles_;
    std::optional<Selection> selectionDrawn_;
    std::array<Com<IDWriteTextFormat>, 7> fonts_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    std::size_t current_{};
    int tabFirst_{};
    std::vector<Target> hits_;
    BitmapCache bitmaps_, beforeBitmaps_;
    std::unordered_map<const void*, Thumbnail> thumbs_;
    std::uint64_t frame_{};
    std::uint64_t compareId_{};
    int lod_{};
    int sidebarTab_{};
    int colorTab_{};
    bool showRulers_ = true;
    bool showPanels_ = true;
    bool showRightPanels_ = true;
    bool showContextBar_ = true;
    bool cloneMerged_ = true;
    bool wandContiguous_ = true;
    bool wandMerged_ = false;
    bool showExtras_ = true;
    bool showGuides_ = true;
    bool guidesLocked_ = false;
    bool snap_ = true;
    bool autoSelect_ = false;
    bool selectionTimer_ = false;
    bool space_ = false;
    bool overWindow_ = false;
    bool tooltip_ = false;
    bool zoomOutMode_ = false;
    float dpi_ = 1;
    int screenMode_ = 0;
    WINDOWPLACEMENT standardPlacement_{};
    float w_{}, h_{};
    Box viewport_{}, sidebar_{}, saturation_{}, hue_{};
    Tool tool_ = Tool::brush;
    std::array<std::optional<Tool>, 26> preferredTools_;
    SelectMode selectionMode_ = SelectMode::replace;
    SelectMode gestureMode_ = SelectMode::replace;
    std::vector<Point> lassoPath_;
    Pixel foreground_ = rgba(0, 0, 0);
    Pixel background_ = rgba(255, 255, 255);
    float hueValue_ = 0.073f, sat_ = 0.55f, val_ = 0.89f;
    int brushSize_ = 48, hardness_ = 80, flow_ = 100, typeSize_ = 72, tolerance_ = 24, wandTolerance_ = 32;
    int created_{};
    Drag drag_ = Drag::none;
    int draggedLayer_ = -1;
    int dropLayer_ = -1;
    Vec cursor_{-100, -100}, down_{}, last_{}, panStart_{}, lastStamp_{};
    Point moveStart_{}, anchor_{};
    Rect moveBounds_{};
    Guide guideDraft_{};
    std::optional<int> movingGuide_;
    Point cloneOffset_{};
    Transform transformStart_{};
    int transformHandle_ = -1;
    std::optional<Rect> gesture_;
    float strokeCarry_{};
    std::size_t memory_{};
};
} // namespace pf
