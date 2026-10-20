#include "bg3vr/vrmenu.hpp"

#include "bg3vr/log.hpp"
#include "bg3vr/text.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite_1.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <mutex>
#include <thread>

namespace bg3vr::vrmenu {

namespace {

using Kind = Snapshot::Row::Kind;

// Layout (pixels of the kWidth x kHeight image).
constexpr int kHeaderBottom = 184;
constexpr int kTabsHeight = 78;
constexpr int kChoiceHeight = 62;
constexpr int kButtonsHeight = 110;
constexpr int kFooterTop = kHeight - 62;
constexpr int kButtonsTop = kFooterTop - kButtonsHeight - 8;
constexpr float kTabGap = 12.0f;
constexpr float kArrowLeft = 600.0f;   // "<" of a choice row
constexpr float kArrowRight = 916.0f;  // ">" of a choice row
constexpr float kArrowWidth = 40.0f;
constexpr float kButtonsLeft = 70.0f;
constexpr float kButtonsRight = kWidth - 70.0f;
constexpr float kButtonGap = 24.0f;

bool parse_float(const std::string& s, float& out) {
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtof(s.c_str(), &end);
    return end && *end == '\0';
}

bool truthy(const std::string& s) {
    const std::string v = text::to_lower(s);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

} // namespace

Menu::Menu() {
    // Each section is a page, shown by its tab.
    const auto section = [&](const char* label) { pages_.push_back(label); };
    const auto page = [&] { return static_cast<int>(pages_.size()) - 1; };
    const auto choice = [&](const char* label, const char* setting, std::vector<Option> options, int fallback) {
        Item item;
        item.page = page();
        item.label = label;
        item.setting = setting;
        item.options = std::move(options);
        item.fallback = fallback;
        item.current = fallback;
        items_.push_back(std::move(item));
    };
    const auto on_off = [&](const char* label, const char* setting, bool fallback) {
        choice(label, setting, {{"On", "true", false}, {"Off", "false", true}}, fallback ? 0 : 1);
    };
    const auto live = [&](const char* label, Item::Live which, std::vector<Option> options) {
        Item item;
        item.page = page();
        item.label = label;
        item.live = which;
        item.options = std::move(options);
        items_.push_back(std::move(item));
    };

    section("View");
    live("Perspective", Item::Live::View, {{"First person", "first"}, {"Third person", "third"}});
    live("Game interface", Item::Live::Ui, {{"In front of you", "world"}, {"On your wrist", "wrist"}});
    choice("Wrist interface size", "ui.wrist_width",
           {{"Small", "0.24"}, {"Medium", "0.3"}, {"Large", "0.36"}, {"Huge", "0.44"}}, 1);
    section("Comfort");
    choice("Turning", "vr.turn", {{"Snap", "snap"}, {"Smooth", "smooth"}}, 0);
    choice("Snap angle", "vr.snap_degrees",
           {{"15°", "15"}, {"30°", "30"}, {"45°", "45"}, {"60°", "60"}, {"90°", "90"}}, 1);
    choice("Smooth turn speed", "vr.smooth_degrees_per_second",
           {{"60°/s", "60"}, {"90°/s", "90"}, {"120°/s", "120"}, {"180°/s", "180"}, {"240°/s", "240"}},
           2);
    choice("Comfort vignette", "vr.comfort_vignette",
           {{"Off", "0", true}, {"Light", "0.25"}, {"Medium", "0.5"}, {"Strong", "0.8"}}, 2);
    on_off("Stick movement", "vr.stick_locomotion", true);
    section("Body");
    on_off("Arms follow controllers", "ik.arms", true);
    choice("Weapon angle", "ik.weapon_pitch",
           {{"\u221260\u00B0", "-60"}, {"\u221245\u00B0", "-45"}, {"\u221230\u00B0", "-30"}, {"\u221215\u00B0", "-15"},
            {"Straight", "0"}, {"+15\u00B0", "15"}, {"+30\u00B0", "30"}, {"+45\u00B0", "45"}, {"+60\u00B0", "60"}},
           4);
    on_off("Point to select", "vr.hand_pointing", true);
    on_off("Grab objects", "grab.hold", false);
    on_off("Physical melee", "combat.physical", false);
    on_off("Holsters", "holster.enabled", true);
    section("Scale");
    choice("IPD (eye distance)", "xr.ipd_mm",
           {{"Headset", "0"}, {"56 mm", "56"}, {"58 mm", "58"}, {"60 mm", "60"}, {"62 mm", "62"}, {"64 mm", "64"},
            {"66 mm", "66"}, {"68 mm", "68"}, {"70 mm", "70"}, {"72 mm", "72"}},
           0);
    choice("World scale", "xr.world_scale",
           {{"80%", "0.8"}, {"90%", "0.9"}, {"95%", "0.95"}, {"100% (true)", "1"}, {"105%", "1.05"}, {"110%", "1.1"},
            {"120%", "1.2"}},
           3);
    choice("Eye height", "vr.eye_offset",
           {{"−10 cm", "-0.1"}, {"−5 cm", "-0.05"}, {"At the eyes", "0"}, {"+5 cm", "0.05"}, {"+10 cm", "0.1"},
            {"+15 cm", "0.15"}, {"+20 cm", "0.2"}},
           2);
    Item buttons;
    buttons.kind = Kind::Buttons;
    buttons.buttons = {"Recentre", "Calibrate height", "Close"};
    items_.push_back(std::move(buttons));
    Item tabs;
    tabs.kind = Kind::Tabs;
    tabs.buttons = pages_;
    items_.insert(items_.begin(), std::move(tabs));
    layout();
}

void Menu::layout() {
    int y = kHeaderBottom;
    for (Item& item : items_) {
        if (item.kind == Kind::Tabs) {
            item.top = kHeaderBottom;
            item.height = kTabsHeight;
            y = kHeaderBottom + kTabsHeight + 8;
        } else if (item.kind == Kind::Buttons) {
            item.top = kButtonsTop;
            item.height = kButtonsHeight;
        } else if (item.page == page_) {
            item.top = y;
            item.height = kChoiceHeight;
            y += kChoiceHeight;
        } else {
            item.top = -10000; // on another page
            item.height = 0;
        }
    }
}

void Menu::show_page(int page) {
    if (page >= 0 && page < static_cast<int>(pages_.size()) && page != page_) {
        page_ = page;
        layout();
        ++revision_;
    }
}

int Menu::page_of(const std::string& label) const {
    for (const Item& item : items_) {
        if (item.label == label) {
            return item.page;
        }
    }
    return -1;
}

void Menu::load(const Config& config) {
    for (Item& item : items_) {
        if (item.setting.empty()) {
            continue;
        }
        const auto dot = item.setting.find('.');
        const std::string raw = config.get(item.setting.substr(0, dot), item.setting.substr(dot + 1));
        item.current = item.fallback;
        if (raw.empty()) {
            continue;
        }
        float number = 0.0f;
        if (item.options.front().value == "true") {
            item.current = truthy(raw) ? 0 : 1;
        } else if (parse_float(raw, number)) {
            float best = 1e30f;
            for (int i = 0; i < static_cast<int>(item.options.size()); ++i) {
                float option = 0.0f;
                if (parse_float(item.options[i].value, option) && std::abs(option - number) < best) {
                    best = std::abs(option - number);
                    item.current = i;
                }
            }
        } else {
            for (int i = 0; i < static_cast<int>(item.options.size()); ++i) {
                if (text::to_lower(item.options[i].value) == text::to_lower(raw)) {
                    item.current = i;
                }
            }
        }
    }
    ++revision_;
}

int Menu::current_index(const Item& item) const {
    switch (item.live) {
    case Item::Live::View:
        return live_.third_person ? 1 : 0;
    case Item::Live::Ui:
        return live_.ui_wrist ? 1 : 0;
    default:
        return item.current;
    }
}

int Menu::row_at(float v) const {
    const float y = v * static_cast<float>(kHeight);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& item = items_[i];
        if (item.kind != Kind::Section && item.height > 0 && y >= static_cast<float>(item.top) &&
            y < static_cast<float>(item.top + item.height)) {
            return i;
        }
    }
    return -1;
}

int Menu::button_at(int row, float u) const {
    if (row < 0 || row >= static_cast<int>(items_.size()) ||
        (items_[row].kind != Kind::Buttons && items_[row].kind != Kind::Tabs)) {
        return -1;
    }
    const int count = static_cast<int>(items_[row].buttons.size());
    const float gap = items_[row].kind == Kind::Tabs ? kTabGap : kButtonGap;
    const float x = u * static_cast<float>(kWidth);
    const float width = (kButtonsRight - kButtonsLeft - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
    for (int b = 0; b < count; ++b) {
        const float left = kButtonsLeft + static_cast<float>(b) * (width + gap);
        if (x >= left && x < left + width) {
            return b;
        }
    }
    return -1;
}

void Menu::step(int index, int direction, Actions& actions) {
    Item& item = items_[index];
    if (item.kind != Kind::Choice || item.options.empty()) {
        return;
    }
    if (item.live == Item::Live::View) {
        actions.toggle_view = true;
        return;
    }
    if (item.live == Item::Live::Ui) {
        actions.toggle_ui = true;
        return;
    }
    const int count = static_cast<int>(item.options.size());
    item.current = (item.current + direction + count) % count;
    actions.settings.emplace_back(item.setting, item.options[item.current].value);
    if (item.setting == "vr.eye_offset") {
        actions.recalibrate = true;
    }
    ++revision_;
}

Actions Menu::update(std::optional<std::array<float, 2>> pointer, bool click, float stick_x, const Live& live) {
    Actions actions;
    if (live.third_person != live_.third_person || live.ui_wrist != live_.ui_wrist) {
        live_ = live;
        ++revision_;
    }
    const int row = pointer ? row_at((*pointer)[1]) : -1;
    const int button = pointer ? button_at(row, (*pointer)[0]) : -1;
    if (row != hovered_ || button != hovered_button_) {
        if (row >= 0 && (row != hovered_ || button != hovered_button_) &&
            ((items_[row].kind != Kind::Buttons && items_[row].kind != Kind::Tabs) || button >= 0)) {
            actions.haptic = 0.08f;
        }
        hovered_ = row;
        hovered_button_ = button;
        ++revision_;
    }
    if (click && row >= 0) {
        const Item& item = items_[row];
        if (item.kind == Kind::Tabs) {
            if (button >= 0) {
                show_page(button);
                actions.haptic = 0.25f;
            }
        } else if (item.kind == Kind::Buttons) {
            switch (button) {
            case 0:
                actions.recenter = true;
                break;
            case 1:
                actions.recalibrate = true;
                break;
            case 2:
                actions.close = true;
                break;
            default:
                break;
            }
            if (button >= 0) {
                actions.haptic = 0.25f;
            }
        } else if (item.kind == Kind::Choice) {
            const float x = (*pointer)[0] * static_cast<float>(kWidth);
            const bool back = x >= kArrowLeft - 20.0f && x < kArrowLeft + kArrowWidth + 20.0f;
            step(row, back ? -1 : 1, actions);
            actions.haptic = 0.25f;
        }
    }
    // The stick changes the row under the laser (on the tabs: the page).
    if (row >= 0 && (items_[row].kind == Kind::Choice || items_[row].kind == Kind::Tabs)) {
        if (stick_armed_ && std::abs(stick_x) > 0.7f) {
            if (items_[row].kind == Kind::Tabs) {
                const int count = static_cast<int>(pages_.size());
                show_page((page_ + (stick_x > 0.0f ? 1 : -1) + count) % count);
            } else {
                step(row, stick_x > 0.0f ? 1 : -1, actions);
            }
            actions.haptic = 0.2f;
            stick_armed_ = false;
        }
    }
    if (std::abs(stick_x) < 0.3f) {
        stick_armed_ = true;
    }
    return actions;
}

Snapshot Menu::snapshot() const {
    Snapshot s;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& item = items_[i];
        if (item.height <= 0) {
            continue; // on another page
        }
        Snapshot::Row row;
        row.kind = item.kind;
        row.label = item.label;
        row.top = item.top;
        row.height = item.height;
        row.hovered = i == hovered_;
        if (item.kind == Kind::Choice && !item.options.empty()) {
            const Option& option = item.options[static_cast<std::size_t>(current_index(item))];
            row.value = option.text;
            row.off = option.off;
        }
        if (item.kind == Kind::Buttons || item.kind == Kind::Tabs) {
            row.buttons = item.buttons;
            row.hovered_button = i == hovered_ ? hovered_button_ : -1;
            row.selected_button = item.kind == Kind::Tabs ? page_ : -1;
        }
        s.rows.push_back(std::move(row));
    }
    s.footer = "Point and pull the trigger  ·  stick ◂ ▸ changes a row  ·  menu button closes";
    return s;
}

std::string Menu::value_of(const std::string& label) const {
    for (const Item& item : items_) {
        if (item.label == label && item.kind == Kind::Choice && !item.options.empty()) {
            return item.options[static_cast<std::size_t>(current_index(item))].text;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------
// Renderer

namespace {

template <typename T> void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

D2D1_COLOR_F rgb(std::uint32_t hex, float alpha = 1.0f) {
    return {static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f, alpha};
}

// The palette: dark leather and old gold.
constexpr std::uint32_t kGold = 0xD9B46C;
constexpr std::uint32_t kGoldBright = 0xF2D58F;
constexpr std::uint32_t kGoldDim = 0x8C6D3C;
constexpr std::uint32_t kText = 0xECE2CC;
constexpr std::uint32_t kTextDim = 0x9E9078;
constexpr std::uint32_t kSection = 0xB8975A;

std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

} // namespace

struct Renderer::Impl {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Snapshot> pending;
    bool quit = false;
    std::vector<std::uint32_t> done;
    std::uint64_t done_seq = 0;
    std::uint64_t taken_seq = 0;
    int init = 0; // 0 waiting, 1 ready, -1 failed

    // Owned by the thread.
    ID2D1Factory* factory = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1DCRenderTarget* target = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1LinearGradientBrush* background = nullptr;
    IDWriteTextFormat* title = nullptr;
    IDWriteTextFormat* subtitle = nullptr;
    IDWriteTextFormat* section = nullptr;
    IDWriteTextFormat* label = nullptr;
    IDWriteTextFormat* value = nullptr;
    IDWriteTextFormat* arrow = nullptr;
    IDWriteTextFormat* button = nullptr;
    IDWriteTextFormat* footer = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    void* bits = nullptr;

    bool create(std::string& error);
    void destroy();
    void draw(const Snapshot& s);
    void run();

    IDWriteTextFormat* format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                              DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        IDWriteTextFormat* f = nullptr;
        if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f))) {
            return nullptr;
        }
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        return f;
    }

    void text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& rect, D2D1_COLOR_F color) {
        brush->SetColor(color);
        target->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, rect, brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    // Spaced capitals (the section headings); returns the right edge of the text.
    float spaced(const std::wstring& s, IDWriteTextFormat* f, float x, float top, float bottom, D2D1_COLOR_F color) {
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 900.0f, bottom - top, &layout))) {
            return x;
        }
        IDWriteTextLayout1* layout1 = nullptr;
        if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&layout1)))) {
            layout1->SetCharacterSpacing(0.0f, 4.0f, 0.0f, {0, static_cast<UINT32>(s.size())});
            layout1->Release();
        }
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        brush->SetColor(color);
        target->DrawTextLayout({x, top}, layout, brush);
        layout->Release();
        return x + metrics.widthIncludingTrailingWhitespace;
    }

    void diamond(float x, float y, float r, D2D1_COLOR_F color) {
        ID2D1PathGeometry* path = nullptr;
        if (FAILED(factory->CreatePathGeometry(&path))) {
            return;
        }
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure({x, y - r}, D2D1_FIGURE_BEGIN_FILLED);
            const D2D1_POINT_2F points[] = {{x + r, y}, {x, y + r}, {x - r, y}};
            sink->AddLines(points, 3);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            brush->SetColor(color);
            target->FillGeometry(path, brush);
        }
        path->Release();
    }

    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F color, float width) {
        brush->SetColor(color);
        target->DrawLine({x0, y0}, {x1, y1}, brush, width);
    }
};

bool Renderer::Impl::create(std::string& error) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) {
        error = "D2D1CreateFactory failed";
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite)))) {
        error = "DWriteCreateFactory failed";
        return false;
    }
    D2D1_RENDER_TARGET_PROPERTIES props{};
    props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    props.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    if (FAILED(factory->CreateDCRenderTarget(&props, &target))) {
        error = "CreateDCRenderTarget failed";
        return false;
    }
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // ClearType needs an opaque target
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kWidth;
    info.bmiHeader.biHeight = -kHeight; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap || !bits) {
        error = "CreateDIBSection failed";
        return false;
    }
    old_bitmap = SelectObject(dc, bitmap);
    const RECT rect{0, 0, kWidth, kHeight};
    if (FAILED(target->BindDC(dc, &rect))) {
        error = "BindDC failed";
        return false;
    }
    if (FAILED(target->CreateSolidColorBrush(rgb(kText), &brush))) {
        error = "CreateSolidColorBrush failed";
        return false;
    }
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, rgb(0x2B2016, 0.97f)}, {0.45f, rgb(0x1A130D, 0.97f)}, {1.0f, rgb(0x0C0906, 0.97f)}};
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(target->CreateGradientStopCollection(stops, 3, &collection))) {
        error = "CreateGradientStopCollection failed";
        return false;
    }
    const HRESULT hr = target->CreateLinearGradientBrush({{0.0f, 0.0f}, {0.0f, static_cast<float>(kHeight)}}, collection, &background);
    collection->Release();
    if (FAILED(hr)) {
        error = "CreateLinearGradientBrush failed";
        return false;
    }
    // Palatino for the old-book headings (DirectWrite falls back to a system font if it is missing), Segoe UI for
    // what must be read at a glance.
    title = format(L"Palatino Linotype", 60.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    subtitle = format(L"Palatino Linotype", 23.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC, DWRITE_TEXT_ALIGNMENT_CENTER);
    section = format(L"Palatino Linotype", 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    label = format(L"Segoe UI", 29.0f, DWRITE_FONT_WEIGHT_NORMAL);
    value = format(L"Segoe UI", 28.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    arrow = format(L"Palatino Linotype", 46.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    button = format(L"Palatino Linotype", 28.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    footer = format(L"Segoe UI", 19.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (!title || !subtitle || !section || !label || !value || !arrow || !button || !footer) {
        error = "CreateTextFormat failed";
        return false;
    }
    return true;
}

void Renderer::Impl::destroy() {
    for (IDWriteTextFormat** f : {&title, &subtitle, &section, &label, &value, &arrow, &button, &footer}) {
        release(*f);
    }
    release(background);
    release(brush);
    release(target);
    release(dwrite);
    release(factory);
    if (dc) {
        if (old_bitmap) {
            SelectObject(dc, old_bitmap);
        }
        DeleteDC(dc);
        dc = nullptr;
    }
    if (bitmap) {
        DeleteObject(bitmap);
        bitmap = nullptr;
    }
}

void Renderer::Impl::draw(const Snapshot& s) {
    const float w = static_cast<float>(kWidth);
    const float h = static_cast<float>(kHeight);
    target->BeginDraw();
    target->SetTransform(D2D1::IdentityMatrix());
    target->Clear(D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.0f});

    // The frame: leather, a gold rule, a thin inner rule with diamonds at its corners.
    const D2D1_ROUNDED_RECT outer{{8.0f, 8.0f, w - 8.0f, h - 8.0f}, 30.0f, 30.0f};
    target->FillRoundedRectangle(outer, background);
    brush->SetColor(rgb(kGold));
    target->DrawRoundedRectangle(outer, brush, 3.0f);
    brush->SetColor(rgb(kGoldDim, 0.8f));
    target->DrawRoundedRectangle({{24.0f, 24.0f, w - 24.0f, h - 24.0f}, 20.0f, 20.0f}, brush, 1.5f);
    for (const auto& [x, y] : {std::pair{40.0f, 40.0f}, {w - 40.0f, 40.0f}, {40.0f, h - 40.0f}, {w - 40.0f, h - 40.0f}}) {
        diamond(x, y, 8.0f, rgb(kGold));
    }

    // Title.
    text(L"VR SETTINGS", title, {0.0f, 46.0f, w, 122.0f}, rgb(kGoldBright));
    text(L"Baldur’s Gate 3  ·  Virtual Reality", subtitle, {0.0f, 116.0f, w, 150.0f}, rgb(kTextDim));
    line(130.0f, 170.0f, w * 0.5f - 22.0f, 170.0f, rgb(kGoldDim), 1.5f);
    line(w * 0.5f + 22.0f, 170.0f, w - 130.0f, 170.0f, rgb(kGoldDim), 1.5f);
    diamond(w * 0.5f, 170.0f, 9.0f, rgb(kGold));
    diamond(124.0f, 170.0f, 3.5f, rgb(kGoldDim));
    diamond(w - 124.0f, 170.0f, 3.5f, rgb(kGoldDim));

    for (const Snapshot::Row& row : s.rows) {
        const float top = static_cast<float>(row.top);
        const float bottom = top + static_cast<float>(row.height);
        const float mid = (top + bottom) * 0.5f;
        switch (row.kind) {
        case Kind::Section: {
            std::wstring caption = widen(row.label);
            std::transform(caption.begin(), caption.end(), caption.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
            const float right = spaced(caption, section, 64.0f, top + 14.0f, bottom, rgb(kSection));
            line(right + 14.0f, mid + 7.0f, w - 64.0f, mid + 7.0f, rgb(kGoldDim, 0.55f), 1.0f);
            break;
        }
        case Kind::Choice: {
            if (row.hovered) {
                brush->SetColor(rgb(kGold, 0.14f));
                target->FillRoundedRectangle({{48.0f, top + 3.0f, w - 48.0f, bottom - 3.0f}, 10.0f, 10.0f}, brush);
                brush->SetColor(rgb(kGold));
                target->FillRectangle({48.0f, top + 10.0f, 53.0f, bottom - 10.0f}, brush);
            } else {
                line(78.0f, bottom, w - 78.0f, bottom, rgb(kGoldDim, 0.18f), 1.0f);
            }
            text(widen(row.label), label, {78.0f, top, kArrowLeft - 10.0f, bottom}, rgb(row.hovered ? kGoldBright : kText));
            const D2D1_COLOR_F arrows = rgb(row.hovered ? kGold : kGoldDim);
            text(L"‹", arrow, {kArrowLeft, top - 4.0f, kArrowLeft + kArrowWidth, bottom - 4.0f}, arrows);
            text(L"›", arrow, {kArrowRight, top - 4.0f, kArrowRight + kArrowWidth, bottom - 4.0f}, arrows);
            text(widen(row.value), value, {kArrowLeft + kArrowWidth, top, kArrowRight, bottom},
                 row.off ? rgb(kTextDim) : rgb(kGoldBright));
            break;
        }
        case Kind::Tabs: {
            const int count = static_cast<int>(row.buttons.size());
            const float bw = (kButtonsRight - kButtonsLeft - kTabGap * static_cast<float>(count - 1)) / static_cast<float>(count);
            for (int b = 0; b < count; ++b) {
                const float left = kButtonsLeft + static_cast<float>(b) * (bw + kTabGap);
                const D2D1_RECT_F r{left, top + 10.0f, left + bw, bottom - 12.0f};
                const bool selected = row.selected_button == b;
                const bool hot = row.hovered_button == b;
                if (selected || hot) {
                    brush->SetColor(rgb(kGold, selected ? 0.20f : 0.10f));
                    target->FillRoundedRectangle({r, 10.0f, 10.0f}, brush);
                }
                if (selected) {
                    brush->SetColor(rgb(kGoldBright));
                    target->FillRectangle({left + 18.0f, r.bottom - 4.0f, left + bw - 18.0f, r.bottom - 1.0f}, brush);
                }
                std::wstring caption = widen(row.buttons[static_cast<std::size_t>(b)]);
                std::transform(caption.begin(), caption.end(), caption.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
                text(caption, button, r, rgb(selected ? kGoldBright : hot ? kGold : kSection));
            }
            line(64.0f, bottom, w - 64.0f, bottom, rgb(kGoldDim, 0.55f), 1.0f);
            break;
        }
        case Kind::Buttons: {
            const int count = static_cast<int>(row.buttons.size());
            const float bw = (kButtonsRight - kButtonsLeft - kButtonGap * static_cast<float>(count - 1)) / static_cast<float>(count);
            for (int b = 0; b < count; ++b) {
                const float left = kButtonsLeft + static_cast<float>(b) * (bw + kButtonGap);
                const D2D1_ROUNDED_RECT r{{left, top + 26.0f, left + bw, bottom - 12.0f}, 12.0f, 12.0f};
                const bool hot = row.hovered_button == b;
                brush->SetColor(hot ? rgb(kGold, 0.24f) : rgb(0x1F1710, 0.9f));
                target->FillRoundedRectangle(r, brush);
                brush->SetColor(rgb(hot ? kGoldBright : kGoldDim));
                target->DrawRoundedRectangle(r, brush, hot ? 2.5f : 1.5f);
                text(widen(row.buttons[static_cast<std::size_t>(b)]), button, r.rect, rgb(hot ? kGoldBright : kGold));
            }
            break;
        }
        }
    }

    line(160.0f, static_cast<float>(kFooterTop) - 6.0f, w - 160.0f, static_cast<float>(kFooterTop) - 6.0f, rgb(kGoldDim, 0.4f), 1.0f);
    text(widen(s.footer), footer, {0.0f, static_cast<float>(kFooterTop), w, static_cast<float>(kFooterTop) + 34.0f}, rgb(kTextDim));
    const HRESULT hr = target->EndDraw();
    if (FAILED(hr)) {
        log::warn("vr menu: drawing failed (0x{:08X})", static_cast<unsigned>(hr));
        return;
    }
    GdiFlush();
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kWidth) * kHeight);
    std::memcpy(pixels.data(), bits, pixels.size() * sizeof(std::uint32_t));
    std::scoped_lock lock(mutex);
    done = std::move(pixels);
    ++done_seq;
}

void Renderer::Impl::run() {
    std::string error;
    const bool ok = create(error);
    {
        std::scoped_lock lock(mutex);
        init = ok ? 1 : -1;
    }
    wake.notify_all();
    if (!ok) {
        log::error("vr menu: can't draw the menu: {}", error);
        destroy();
        return;
    }
    for (;;) {
        Snapshot snapshot;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return quit || pending.has_value(); });
            if (quit) {
                break;
            }
            snapshot = std::move(*pending);
            pending.reset();
        }
        draw(snapshot);
    }
    destroy();
}

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}

Renderer::~Renderer() {
    if (impl_->thread.joinable()) {
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->quit = true;
        }
        impl_->wake.notify_all();
        impl_->thread.join();
    }
}

bool Renderer::start() {
    if (!impl_->thread.joinable()) {
        impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    }
    std::unique_lock lock(impl_->mutex);
    impl_->wake.wait(lock, [&] { return impl_->init != 0; });
    return impl_->init > 0;
}

void Renderer::submit(Snapshot snapshot) {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->pending = std::move(snapshot);
    }
    impl_->wake.notify_all();
}

bool Renderer::take(std::vector<std::uint32_t>& pixels) {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->done_seq == impl_->taken_seq || impl_->done.empty()) {
        return false;
    }
    impl_->taken_seq = impl_->done_seq;
    pixels = impl_->done;
    return true;
}

} // namespace bg3vr::vrmenu
