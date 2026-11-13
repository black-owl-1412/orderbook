// Live order-book viewer: last trade price, top 5 levels per side, trade and reject counts,
// a live price chart, and an order ticket for placing your own orders.
//   ./build/orderbook_viz [--reduce-motion] [--speed N] [--scenario Name] [--raw-book]
//   ./build/orderbook_viz --screenshot out.ppm [frames=240]   render, save one frame, exit
//   ./build/orderbook_viz --demo-orders   place a few example user orders after warm-up

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "LiveSim.h"

using namespace orderbook;

#if defined(__SANITIZE_ADDRESS__)
// In ASan builds the system GUI stack (fontconfig, GTK window decorations, the Mesa
// driver, which is unloaded before the leak check runs) never frees some allocations,
// so LeakSanitizer reports them at exit. Leak checking is turned off for the viewer
// only; ASan still checks every memory access, and the tests keep leak detection.
extern "C" const char* __asan_default_options() { return "detect_leaks=0"; }
#endif

namespace {

// --------------------------------------------------------------- tokens ----

// Gruvbox Dark palette (github.com/morhetz/gruvbox), mapped onto the viewer's roles.
namespace color {
constexpr ImVec4 Hex(unsigned rgb) {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f, 1.0f};
}
constexpr ImVec4 Background = Hex(0x1d2021);    // bg0_h   surface 0: window
constexpr ImVec4 Card = Hex(0x282828);          // bg0     surface 1: cards
constexpr ImVec4 CardBorder = Hex(0x32302f);    // bg0_s
constexpr ImVec4 Control = Hex(0x3c3836);       // bg1     surface 2: controls
constexpr ImVec4 ControlHover = Hex(0x504945);  // bg2
constexpr ImVec4 Shadow{0.0f, 0.0f, 0.0f, 1.0f};
constexpr ImVec4 Text = Hex(0xebdbb2);          // fg1     values
constexpr ImVec4 OnAccent = Hex(0x282828);      // bg0     text on an accent-filled button
constexpr ImVec4 Dim = Hex(0xa89984);           // fg4     labels only (5.3:1 on Card; gray is 4.0:1)
constexpr ImVec4 Accent = Hex(0x83a598);        // blue    interactive controls
constexpr ImVec4 Bid = Hex(0xb8bb26);           // green   bid / uptick only
constexpr ImVec4 Ask = Hex(0xfb4934);           // red     ask / downtick only
}  // namespace color

// Sizes in logical pixels; Px() scales them for the display (HiDPI).
namespace metric {
constexpr float WindowWidth = 1080, WindowHeight = 900;
constexpr float RightColumnWidth = 420;  // order ticket + my orders
constexpr float FieldLabelWidth = 56;    // "Price", "Qty" column in the ticket
constexpr float BestButtonWidth = 52;
constexpr float StatusDot = 3.5f;        // radius of the status / your-order dots
constexpr float PressAlpha = 0.30f, HoverAlpha = 0.10f;  // ladder price cell feedback
constexpr float BodyFont = 16, SmallFont = 13, BigFont = 34;
constexpr float CaptionTracking = 0.8f;  // extra advance per glyph for small uppercase labels
constexpr float CellPad = 6;             // inner padding of numbers in ladder cells
constexpr float BarInset = 3, BarRounding = 3, BarAlpha = 0.22f;
constexpr float ArrowSize = 18, ArrowGap = 12;
constexpr float ComboWidth = 130, SeedWidth = 110, PlayWidth = 80;
constexpr float ChartMinHeight = 140;
constexpr float BandAlpha = 0.18f;
constexpr int ShadowLayers = 4;
constexpr float ShadowSpread = 2.5f, ShadowOffsetY = 2, ShadowAlpha = 0.07f;
}  // namespace metric

// Motion (see the skill's house values). Data is never animated, only decoration.
namespace motion {
constexpr float BarResponse = 0.18f;       // depth bar length, critically damped
constexpr float FastBarResponse = 0.08f;   // ... when the book changes a lot per frame
constexpr float ScaleResponse = 0.4f;      // max quantity the bars are normalised to
constexpr float ChartResponse = 0.5f;      // chart y-range
constexpr double FlashSeconds = 0.7;       // price flash, ease-out
constexpr int FastSpeed = 200;             // events/frame: use FastBarResponse above this
constexpr int SnapSpeed = 2'000;           // events/frame: bars snap above this (motion = noise)
}  // namespace motion

constexpr std::size_t kDepth = 5;
constexpr std::size_t kHistory = 600;  // chart samples, one per frame (~10 s at 60 fps)
constexpr int kDemoOrdersFrame = 90;   // --demo-orders: frame at which the examples are sent

// Order types offered in the ticket, in display order, with a one-line explanation.
struct TicketType {
    OrderType type;
    const char* help;
};
constexpr std::array kTicketTypes{
    TicketType{OrderType::GoodTillCancel, "good till cancel: rests until filled or cancelled"},
    TicketType{OrderType::GoodForDay, "good for day: like GTC, expires at end of day"},
    TicketType{OrderType::FillAndKill, "fill and kill: fill what you can now, cancel the rest"},
    TicketType{OrderType::FillOrKill, "fill or kill: fill everything now, or reject"},
    TicketType{OrderType::Market, "market: any price, cancel the rest"},
};

// Short reject labels, shared by the stats card and the orders list.
const char* ShortReason(RejectReason r) {
    switch (r) {
        case RejectReason::CannotMatch: return "no match";
        case RejectReason::UnknownOrderId: return "unknown";
        case RejectReason::CannotFullyFill: return "no fill";
        case RejectReason::DuplicateOrderId: return "dup id";
        case RejectReason::ZeroQuantity: return "zero qty";
        case RejectReason::InvalidOrder: return "invalid";
    }
    std::unreachable();
}

float gScale = 1.0f;  // display content scale, set once at startup
float Px(float logical) { return logical * gScale; }

ImVec4 WithAlpha(ImVec4 c, float a) { return {c.x, c.y, c.z, a}; }
ImVec4 Lerp(ImVec4 a, ImVec4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

// ---------------------------------------------------------------- motion ---

// Frame-rate independent spring (mass 1). Interruptible by construction: a new target
// just continues from the current value and velocity.
struct Spring {
    float value = 0.0f;
    float velocity = 0.0f;

    void Snap(float v) {
        value = v;
        velocity = 0.0f;
    }

    void Step(float target, float dt, float response, float dampingRatio = 1.0f) {
        const float omega = 2.0f * 3.14159265f / response;
        const float k = omega * omega;
        const float c = 2.0f * dampingRatio * omega;
        dt = std::min(dt, 0.1f);  // a stalled frame must not explode the spring
        const int steps = std::max(1, static_cast<int>(std::ceil(dt / 0.004f)));
        const float h = dt / static_cast<float>(steps);
        for (int i = 0; i < steps; ++i) {  // semi-implicit Euler
            velocity += (k * (target - value) - c * velocity) * h;
            value += velocity * h;
        }
    }

    // Step, or snap when motion is off.
    void Follow(float target, float dt, float response, bool animate) {
        if (animate)
            Step(target, dt, response);
        else
            Snap(target);
    }
};

// What motion is allowed this frame.
struct MotionPolicy {
    bool animate = true;       // decoration may move at all (false: reduce motion)
    bool animateBars = true;   // false at very high speed, where bar motion is only noise
    float barResponse = motion::BarResponse;
    float dt = 1.0f / 60.0f;

    static MotionPolicy For(bool reduceMotion, int eventsPerFrame, float dt) {
        MotionPolicy p;
        p.animate = !reduceMotion;
        p.animateBars = p.animate && eventsPerFrame < motion::SnapSpeed;
        p.barResponse = eventsPerFrame > motion::FastSpeed ? motion::FastBarResponse : motion::BarResponse;
        p.dt = dt;
        return p;
    }
};

// One spring per visible price level, keyed by PRICE so that when the book shifts by a
// row a bar never morphs from its neighbour's length. A new level grows from 0. Linear
// search over at most a dozen entries; capacity is reserved once, so no per-frame allocation.
class LevelSprings {
public:
    LevelSprings() { entries_.reserve(4 * kDepth); }

    Spring& At(Price price, std::uint64_t frame) {
        for (Entry& e : entries_)
            if (e.price == price) {
                e.seen = frame;
                return e.spring;
            }
        entries_.push_back({price, Spring{}, frame});
        return entries_.back().spring;
    }

    // Drop levels that are no longer visible (their rows are reused by other prices).
    void Prune(std::uint64_t frame) {
        std::erase_if(entries_, [frame](const Entry& e) { return e.seen != frame; });
    }

    void Clear() { entries_.clear(); }

private:
    struct Entry {
        Price price;
        Spring spring;
        std::uint64_t seen;
    };
    std::vector<Entry> entries_;
};

// ----------------------------------------------------------------- theme ---

struct Fonts {
    ImFont* body = nullptr;   // labels, values, table
    ImFont* small = nullptr;  // captions
    ImFont* big = nullptr;    // headline numbers
};

Fonts LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    Fonts f;
    const std::filesystem::path dir{ORDERBOOK_FONT_DIR};
    const auto regular = (dir / "JetBrainsMono-Regular.ttf").string();
    const auto bold = (dir / "JetBrainsMono-Bold.ttf").string();
    // Baked at the display's scale, so text stays sharp (never SetWindowFontScale).
    if (std::filesystem::exists(regular) && std::filesystem::exists(bold)) {
        f.body = io.Fonts->AddFontFromFileTTF(regular.c_str(), Px(metric::BodyFont));
        f.small = io.Fonts->AddFontFromFileTTF(regular.c_str(), Px(metric::SmallFont));
        f.big = io.Fonts->AddFontFromFileTTF(bold.c_str(), Px(metric::BigFont));
    }
    if (!f.body || !f.small || !f.big) {  // font missing: built-in font at every size
        std::fprintf(stderr, "font not found in %s, using ImGui's default font\n", dir.c_str());
        io.Fonts->Clear();
        ImFontConfig cfg;
        cfg.SizePixels = Px(metric::BodyFont - 1);
        f.body = io.Fonts->AddFontDefault(&cfg);
        cfg.SizePixels = Px(metric::SmallFont);
        f.small = io.Fonts->AddFontDefault(&cfg);
        cfg.SizePixels = Px(metric::BigFont - 4);
        f.big = io.Fonts->AddFontDefault(&cfg);
    }
    return f;
}

void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = {16, 14};
    s.FramePadding = {10, 6};
    s.ItemSpacing = {10, 8};
    s.CellPadding = {8, 3};
    s.WindowRounding = 0;
    s.ChildRounding = 10;
    s.FrameRounding = 6;
    s.PopupRounding = 6;
    s.GrabRounding = 6;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowBorderSize = 0;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = color::Background;
    c[ImGuiCol_ChildBg] = color::Card;
    c[ImGuiCol_PopupBg] = color::Card;
    c[ImGuiCol_Border] = color::CardBorder;
    c[ImGuiCol_Text] = color::Text;
    c[ImGuiCol_TextDisabled] = color::Dim;
    c[ImGuiCol_FrameBg] = color::Control;
    c[ImGuiCol_FrameBgHovered] = color::ControlHover;
    c[ImGuiCol_FrameBgActive] = color::ControlHover;
    c[ImGuiCol_Button] = color::Control;
    c[ImGuiCol_ButtonHovered] = color::ControlHover;
    c[ImGuiCol_ButtonActive] = WithAlpha(color::Accent, 0.6f);  // instant, distinct press feedback
    c[ImGuiCol_Header] = WithAlpha(color::Accent, 0.25f);
    c[ImGuiCol_HeaderHovered] = WithAlpha(color::Accent, 0.35f);
    c[ImGuiCol_HeaderActive] = WithAlpha(color::Accent, 0.45f);
    c[ImGuiCol_CheckMark] = color::Accent;
    c[ImGuiCol_SliderGrab] = color::Accent;
    c[ImGuiCol_SliderGrabActive] = color::Accent;
    c[ImGuiCol_TableHeaderBg] = color::Card;
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = {0, 0, 0, 0};
    c[ImGuiCol_TableBorderLight] = color::CardBorder;
    c[ImGuiCol_Separator] = color::CardBorder;
    s.ScaleAllSizes(gScale);

    ImPlotStyle& p = ImPlot::GetStyle();
    p.PlotPadding = {0, Px(4)};
    p.LineWeight = 1.5f;
    p.Colors[ImPlotCol_FrameBg] = {0, 0, 0, 0};
    p.Colors[ImPlotCol_PlotBg] = {0, 0, 0, 0};
    p.Colors[ImPlotCol_PlotBorder] = {0, 0, 0, 0};
    p.Colors[ImPlotCol_AxisGrid] = WithAlpha(color::CardBorder, 0.8f);
    p.Colors[ImPlotCol_AxisText] = color::Dim;
    p.Colors[ImPlotCol_LegendBg] = WithAlpha(color::Card, 0.85f);
    p.Colors[ImPlotCol_LegendBorder] = {0, 0, 0, 0};
    p.Colors[ImPlotCol_LegendText] = color::Dim;
}

// ------------------------------------------------------------- drawing ----

// UTF-8 sequence length from its first byte.
int Utf8Length(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4; }

// Width of text drawn with extra per-glyph tracking (ImGui has no letter-spacing).
float TrackedWidth(const char* text, float tracking) {
    float w = 0.0f;
    int glyphs = 0;
    for (const char* s = text; *s;) {
        const int n = Utf8Length(static_cast<unsigned char>(*s));
        w += ImGui::CalcTextSize(s, s + n).x;
        ++glyphs;
        s += n;
    }
    return w + tracking * static_cast<float>(std::max(0, glyphs - 1));
}

// Text with extra tracking, at the cursor; advances the cursor like ImGui::Text.
void TrackedText(ImVec4 col, const char* text, float tracking) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = ImGui::GetColorU32(col);
    float x = p.x;
    for (const char* s = text; *s;) {
        const int n = Utf8Length(static_cast<unsigned char>(*s));
        dl->AddText({x, p.y}, c, s, s + n);
        x += ImGui::CalcTextSize(s, s + n).x + tracking;
        s += n;
    }
    ImGui::Dummy({TrackedWidth(text, tracking), ImGui::GetTextLineHeight()});
}

// Small, uppercase, dim, slightly tracked: the label at the top of a card.
void Caption(const Fonts& fonts, const char* text) {
    ImGui::PushFont(fonts.small);
    TrackedText(color::Dim, text, Px(metric::CaptionTracking));
    ImGui::PopFont();
}

// "label value": dim label, value in the value colour, on one line.
void LabelValue(const char* label, const std::string& value, ImVec4 valueColor = color::Text) {
    ImGui::TextColored(color::Dim, "%s", label);
    ImGui::SameLine(0, ImGui::CalcTextSize(" ").x);
    ImGui::TextColored(valueColor, "%s", value.c_str());
}

// Soft shadow behind a card: a few expanding, low-alpha rounded rects.
void DrawCardShadow(ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().ChildRounding;
    const float area = (max.x - min.x) * (max.y - min.y);
    const float sizeBoost = area > Px(600) * Px(300) ? 1.5f : 1.0f;  // bigger surfaces read thicker
    const float dy = Px(metric::ShadowOffsetY) * sizeBoost;
    // Each layer is a RING around the card, not a filled rect: the opaque card hides the
    // inside anyway, and filling it costs a lot under software rendering (measured: a
    // filled shadow took the viewer from 56 to 35 fps). The ring reaches `inner` pixels
    // under the card so the downward offset leaves no gap along the bottom edge.
    const float inner = dy + Px(1);
    for (int i = metric::ShadowLayers; i >= 1; --i) {
        const float grow = Px(metric::ShadowSpread) * static_cast<float>(i) * sizeBoost;
        const float thickness = grow + inner;
        const float centre = (grow - inner) * 0.5f;  // stroke is centred on the path
        dl->AddRect({min.x - centre, min.y - centre + dy}, {max.x + centre, max.y + centre + dy},
                    ImGui::GetColorU32(WithAlpha(color::Shadow, metric::ShadowAlpha)), rounding + centre,
                    ImDrawFlags_None, thickness);
    }
}

// Cards are bordered, rounded child windows on the darker background, lifted by a soft
// shadow. The shadow must be drawn BEFORE the card (drawn after, it lands on top of the
// card's background). An auto-height card only knows its size after it is drawn, so the
// size from the previous frame is used; the layout never moves, so it is always right
// after the first frame.
bool BeginCard(const char* id, ImVec2 size, ImGuiChildFlags extra = 0) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = size.x > 0 ? size.x : storage->GetFloat(key, 0.0f);
    const float height = size.y > 0 ? size.y : storage->GetFloat(key + 1, 0.0f);
    if (width > 0 && height > 0)
        DrawCardShadow(pos, {pos.x + width, pos.y + height});
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | extra,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 actual = ImGui::GetWindowSize();  // the card's size this frame
    storage->SetFloat(key, actual.x);
    storage->SetFloat(key + 1, actual.y);
    return open;
}

void EndCard() { ImGui::EndChild(); }

// Text vertically centred in a table row; align 0 = left, 0.5 = centre, 1 = right,
// with the same inner padding as the depth bars' numbers.
void AlignedText(ImVec4 col, const char* text, float rowHeight, float align, float tracking = 0.0f) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float textWidth = TrackedWidth(text, tracking);
    const float pad = align == 0.5f ? 0.0f : Px(metric::CellPad);
    const float x = p.x + pad + (width - textWidth - 2 * pad) * align;
    ImGui::SetCursorScreenPos({x, p.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f});
    TrackedText(col, text, tracking);
}

// A small filled circle, vertically centred on the current line; advances the cursor.
void Dot(ImVec4 col) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = Px(metric::StatusDot);
    ImGui::GetWindowDrawList()->AddCircleFilled({p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f}, r,
                                                ImGui::GetColorU32(col));
    ImGui::Dummy({2 * r, ImGui::GetTextLineHeight()});
}

// Filled triangle pointing up (+1) or down (-1), vertically centred on the current line.
void DirectionArrow(int direction, ImVec4 col, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    const float cx = p.x + size * 0.5f;
    const float cy = p.y + h * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = ImGui::GetColorU32(col);
    if (direction > 0)
        dl->AddTriangleFilled({cx, cy - size * 0.45f}, {cx + size * 0.5f, cy + size * 0.35f},
                              {cx - size * 0.5f, cy + size * 0.35f}, c);
    else if (direction < 0)
        dl->AddTriangleFilled({cx - size * 0.5f, cy - size * 0.35f}, {cx + size * 0.5f, cy - size * 0.35f},
                              {cx, cy + size * 0.45f}, c);
    ImGui::Dummy({size, h});
}

// ---------------------------------------------------------------- state ----

struct ViewState {
    // Price flash: direction of the last change of the last trade price, and when.
    std::optional<Price> previousLast;
    int direction = 0;  // +1 uptick, -1 downtick
    double changedAt = -1e9;

    // Chart history. bandLo/bandHi is the bid-ask band; when one side is empty it
    // collapses onto the other. last is NaN until the first trade.
    std::vector<double> x, last, bandLo, bandHi;
    double sampleIndex = 0;

    // Decoration motion.
    LevelSprings bidBars, askBars;
    Spring barScale;
    bool barScaleReady = false;
    Spring chartLo, chartHi;
    bool chartReady = false;
    std::uint64_t frame = 0;

    void Clear() {
        previousLast.reset();
        direction = 0;
        changedAt = -1e9;
        for (auto* v : {&x, &last, &bandLo, &bandHi})
            v->clear();
        sampleIndex = 0;
        bidBars.Clear();
        askBars.Clear();
        barScaleReady = chartReady = false;
    }

    // Fires the flash only when the PRICE changes, not on every trade.
    void Observe(const viz::LiveSim& sim, double now) {
        const auto lastPrice = sim.LastTradePrice();
        if (lastPrice && previousLast && *lastPrice != *previousLast) {
            direction = *lastPrice > *previousLast ? 1 : -1;
            changedAt = now;
        }
        previousLast = lastPrice;
    }

    void Sample(const viz::LiveSim& sim) {
        constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        const OrderBook& book = sim.Book();
        const auto bid = book.BestBid();
        const auto ask = book.BestAsk();
        if (!bid && !ask)
            return;  // empty book: nothing to draw
        const auto lastPrice = sim.LastTradePrice();
        x.push_back(sampleIndex++);
        last.push_back(lastPrice ? *lastPrice : nan);
        bandLo.push_back(bid ? *bid : *ask);
        bandHi.push_back(ask ? *ask : *bid);
        if (x.size() > kHistory)
            for (auto* v : {&x, &last, &bandLo, &bandHi})
                v->erase(v->begin());
    }
};

// The order ticket's fields and the outcome of the last order sent.
struct OrderTicket {
    Side side = Side::Buy;
    int typeIndex = 0;  // into kTicketTypes
    int price = 0;
    int qty = 10;
    bool followBest = true;  // price tracks the best price until the user picks one

    std::string result;  // one line under the submit button
    ImVec4 resultColor = color::Dim;

    OrderType Type() const { return kTicketTypes[static_cast<std::size_t>(typeIndex)].type; }

    void UsePrice(Price p) {
        price = p;
        followBest = false;
    }

    // Describes what happened to the order that was just placed.
    void Report(const AddResult& result_, const viz::UserOrder& u) {
        char buf[128];
        if (!result_) {
            std::snprintf(buf, sizeof buf, "rejected: %s", ToString(result_.error()).data());
            result = buf;
            resultColor = color::Ask;  // error
            return;
        }
        double notional = 0;
        for (const Trade& t : *result_)
            notional += static_cast<double>(t.GetPrice()) * t.GetQuantity();
        const double avg = u.filled ? notional / u.filled : 0.0;
        char fill[48] = "";
        if (u.filled)
            std::snprintf(fill, sizeof fill, result_->size() == 1 ? "%u @ %.0f" : "%u @ avg %.1f",
                          u.filled, avg);
        const unsigned seq = static_cast<unsigned>(u.id - viz::kUserIdBase + 1);
        switch (u.status) {
            case viz::UserStatus::Filled:
                std::snprintf(buf, sizeof buf, "#%u filled %s", seq, fill);
                break;
            case viz::UserStatus::Cancelled:
                std::snprintf(buf, sizeof buf, "#%u filled %s, rest cancelled", seq, fill);
                break;
            case viz::UserStatus::PartlyFilled:
                std::snprintf(buf, sizeof buf, "#%u filled %s, %u resting @ %d", seq, fill, u.Remaining(), u.price);
                break;
            default:
                std::snprintf(buf, sizeof buf, "#%u resting %u @ %d", seq, u.qty, u.price);
                break;
        }
        result = buf;
        resultColor = color::Text;  // completion
    }
};

// Sends the ticket's order and records the outcome on the ticket.
void Submit(viz::LiveSim& sim, OrderTicket& t) {
    const auto r = sim.PlaceUserOrder(t.Type(), t.side, t.price, static_cast<Quantity>(std::max(0, t.qty)));
    t.Report(r, sim.UserOrders().back());
}

// --demo-orders: a fixed set of examples, so screenshots can show the feature at work.
void PlaceDemoOrders(viz::LiveSim& sim, OrderTicket& t) {
    const auto bid = sim.Book().BestBid();
    const auto ask = sim.Book().BestAsk();
    if (!bid || !ask)
        return;
    (void)sim.PlaceUserOrder(OrderType::GoodTillCancel, Side::Buy, *bid - 1, 10);  // rests
    (void)sim.PlaceUserOrder(OrderType::GoodForDay, Side::Sell, *ask + 2, 8);       // rests (GFD)
    (void)sim.PlaceUserOrder(OrderType::FillOrKill, Side::Buy, *ask, 5'000);        // rejected
    (void)sim.PlaceUserOrder(OrderType::Market, Side::Sell, 0, 3);                  // fills
    t.side = Side::Buy;
    t.typeIndex = 0;
    t.qty = 5;
    t.UsePrice(*ask);
    Submit(sim, t);  // crosses: filled, shown in the result line
}

// --------------------------------------------------------------- panels ----

void PriceCard(const Fonts& fonts, const viz::LiveSim& sim, const ViewState& view,
               const MotionPolicy& motionPolicy, ImVec2 size) {
    BeginCard("price", size);
    Caption(fonts, "LAST PRICE");

    // Flash: the number starts in the direction colour and eases out to the value colour.
    // Reduce motion: no fade; the arrow alone carries the direction.
    const auto last = sim.LastTradePrice();
    const ImVec4 dirColor = view.direction > 0 ? color::Bid : view.direction < 0 ? color::Ask : color::Dim;
    ImVec4 numberColor = color::Text;
    if (motionPolicy.animate) {
        const double t = std::clamp((ImGui::GetTime() - view.changedAt) / motion::FlashSeconds, 0.0, 1.0);
        const float eased = static_cast<float>(1.0 - std::pow(1.0 - t, 3.0));
        numberColor = Lerp(dirColor, color::Text, eased);
    }

    ImGui::PushFont(fonts.big);
    if (last)
        ImGui::TextColored(numberColor, "%d", *last);  // exact value, never interpolated
    else
        ImGui::TextColored(color::Dim, "--");
    ImGui::SameLine(0, Px(metric::ArrowGap));
    DirectionArrow(view.direction, dirColor, Px(metric::ArrowSize));
    ImGui::PopFont();

    const OrderBook& book = sim.Book();
    const auto bid = book.BestBid();
    const auto ask = book.BestAsk();
    auto priceOrDash = [](const std::optional<Price>& p) {
        return p ? std::to_string(*p) : std::string{"--"};
    };
    LabelValue("Bid", priceOrDash(bid), color::Bid);
    ImGui::SameLine(0, Px(16));
    LabelValue("Ask", priceOrDash(ask), color::Ask);

    char mid[32] = "--";
    if (bid && ask)
        std::snprintf(mid, sizeof mid, "%.1f", 0.5 * (*bid + *ask));
    LabelValue("Mid", mid);
    ImGui::SameLine(0, Px(16));
    LabelValue("Spread", bid && ask ? std::to_string(*ask - *bid) : std::string{"--"});
    EndCard();
}

void StatsCard(const Fonts& fonts, const viz::LiveSim& sim, ImVec2 size) {
    BeginCard("stats", size);
    Caption(fonts, "TRADES");
    ImGui::PushFont(fonts.big);
    ImGui::Text("%llu", static_cast<unsigned long long>(sim.Trades()));
    ImGui::PopFont();

    LabelValue("Rejected", std::to_string(sim.Rejects()));
    // Every reason is always listed (even at zero) so the card never changes height.
    constexpr std::array kReasons{RejectReason::CannotMatch,     RejectReason::UnknownOrderId,
                                  RejectReason::CannotFullyFill, RejectReason::DuplicateOrderId,
                                  RejectReason::ZeroQuantity,    RejectReason::InvalidOrder};
    ImGui::PushFont(fonts.small);
    if (ImGui::BeginTable("reasons", 2, ImGuiTableFlags_SizingStretchSame)) {
        for (const RejectReason r : kReasons) {
            ImGui::TableNextColumn();
            char label[16];
            std::snprintf(label, sizeof label, "%-8s", ShortReason(r));  // pad so counts line up
            LabelValue(label, std::to_string(sim.Rejects(r)));
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    EndCard();
}

// One quantity cell: a bar anchored on the price column (bids grow left, asks grow
// right) at the animated length `fraction`, with the EXACT quantity next to the price
// and, if the user has orders resting at this level, "you N" beside it.
void DepthCell(Quantity qty, Quantity mine, float fraction, bool isBid, float rowHeight) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float barWidth = width * std::clamp(fraction, 0.0f, 1.0f);
    const ImVec4 c = isBid ? color::Bid : color::Ask;
    const float inset = Px(metric::BarInset);
    if (barWidth >= 0.5f) {
        const ImVec2 min{isBid ? p.x + width - barWidth : p.x, p.y + inset};
        const ImVec2 max{isBid ? p.x + width : p.x + barWidth, p.y + rowHeight - inset};
        ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(WithAlpha(c, metric::BarAlpha)),
                                                  Px(metric::BarRounding));
    }
    // Text goes through the draw list, then one line-height Dummy keeps the cell's
    // content inside the row's padding (so filled and empty rows are the same height).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string label = std::to_string(qty);
    const float pad = Px(metric::CellPad);
    const float lineH = ImGui::GetTextLineHeight();
    const float y = p.y + (rowHeight - lineH) * 0.5f;
    const float textW = ImGui::CalcTextSize(label.c_str()).x;
    const float qx = isBid ? p.x + width - pad - textW : p.x + pad;
    dl->AddText({qx, y}, ImGui::GetColorU32(c), label.c_str());
    if (mine > 0) {
        const std::string you = "you " + std::to_string(mine);
        const float youW = ImGui::CalcTextSize(you.c_str()).x;
        const float gap = 2 * pad;
        dl->AddText({isBid ? qx - gap - youW : qx + textW + gap, y}, ImGui::GetColorU32(color::Accent),
                    you.c_str());
    }
    ImGui::SetCursorScreenPos({p.x, y});
    ImGui::Dummy({width, lineH});
}

// A ladder price: clicking it copies the price into the order ticket (it does not
// trade). Highlights on press, commits on release, with a dot when the user has an
// order resting at this level.
void PriceCell(Price price, Side side, bool mine, float rowHeight, OrderTicket& ticket) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float contentH = rowHeight - 2 * ImGui::GetStyle().CellPadding.y;  // stay inside the row
    ImGui::PushID(static_cast<int>(side));
    ImGui::PushID(price);
    if (ImGui::InvisibleButton("##price", {width, contentH}))
        ticket.UsePrice(price);
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || hovered)
        dl->AddRectFilled(p, {p.x + width, p.y + contentH},
                          ImGui::GetColorU32(WithAlpha(color::Accent, active ? metric::PressAlpha : metric::HoverAlpha)),
                          Px(metric::BarRounding));
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Use %d in the order ticket", price);
    }
    if (mine) {
        const float r = Px(metric::StatusDot);
        dl->AddCircleFilled({p.x + r + Px(2), p.y + rowHeight * 0.5f}, r, ImGui::GetColorU32(color::Accent));
    }
    ImGui::SetCursorScreenPos(p);
    AlignedText(color::Text, std::to_string(price).c_str(), rowHeight, 0.5f);
}

void BookCard(const Fonts& fonts, const viz::LiveSim& sim, ViewState& view, OrderTicket& ticket,
              const MotionPolicy& mp, ImVec2 size) {
    BeginCard("book", size, ImGuiChildFlags_AutoResizeY);
    Caption(fonts, "ORDER BOOK  \xc2\xb7  TOP 5 LEVELS");  // UTF-8 middle dot

    const auto infos = sim.Book().GetOrderInfos();  // both sides best-first
    const LevelInfos& bids = infos.GetBids();
    const LevelInfos& asks = infos.GetAsks();
    const std::size_t nBids = std::min(kDepth, bids.size());
    const std::size_t nAsks = std::min(kDepth, asks.size());
    Quantity maxQty = 1;
    for (std::size_t i = 0; i < nBids; ++i) maxQty = std::max(maxQty, bids[i].quantity_);
    for (std::size_t i = 0; i < nAsks; ++i) maxQty = std::max(maxQty, asks[i].quantity_);

    // The scale follows the largest visible level slowly, so one big order arriving does
    // not make every other bar jump.
    ++view.frame;
    if (!view.barScaleReady) {
        view.barScale.Snap(static_cast<float>(maxQty));
        view.barScaleReady = true;
    } else {
        view.barScale.Follow(static_cast<float>(maxQty), mp.dt, motion::ScaleResponse, mp.animateBars);
    }
    auto barFraction = [&](LevelSprings& springs, const LevelInfo& l) {
        Spring& s = springs.At(l.price_, view.frame);
        s.Follow(static_cast<float>(l.quantity_), mp.dt, mp.barResponse, mp.animateBars);
        return std::max(0.0f, s.value) / std::max(1.0f, view.barScale.value);
    };

    if (ImGui::BeginTable("ladder", 3, ImGuiTableFlags_SizingFixedFit)) {
        // Room for a 5-6 digit price plus the your-order dot on its left.
        const float priceWidth = ImGui::CalcTextSize("000000000").x;
        ImGui::TableSetupColumn("bid", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("price", ImGuiTableColumnFlags_WidthFixed, priceWidth);
        ImGui::TableSetupColumn("ask", ImGuiTableColumnFlags_WidthStretch);

        // Fixed row height and placeholder rows: the table is always 12 rows tall.
        const float rowHeight = ImGui::GetFrameHeight();
        const float tracking = Px(metric::CaptionTracking);
        ImGui::PushFont(fonts.small);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
        ImGui::TableSetColumnIndex(0);
        AlignedText(color::Dim, "BIDS", rowHeight, 1.0f, tracking);  // flush right, next to the price
        ImGui::TableSetColumnIndex(1);
        AlignedText(color::Dim, "PRICE", rowHeight, 0.5f, tracking);
        ImGui::TableSetColumnIndex(2);
        AlignedText(color::Dim, "ASKS", rowHeight, 0.0f, tracking);
        ImGui::PopFont();

        for (std::size_t row = 0; row < kDepth; ++row) {  // asks: highest of the 5 at the top
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            if (row < kDepth - nAsks)
                continue;
            const LevelInfo& l = asks[kDepth - 1 - row];
            const Quantity mine = sim.UserRestingAt(Side::Sell, l.price_);
            ImGui::TableSetColumnIndex(1);
            PriceCell(l.price_, Side::Sell, mine > 0, rowHeight, ticket);
            ImGui::TableSetColumnIndex(2);
            DepthCell(l.quantity_, mine, barFraction(view.askBars, l), false, rowHeight);
        }

        {  // spread row
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableSetColumnIndex(1);
            const auto bid = sim.Book().BestBid();
            const auto ask = sim.Book().BestAsk();
            const std::string spread = bid && ask ? "spread " + std::to_string(*ask - *bid) : "spread --";
            ImGui::PushFont(fonts.small);
            AlignedText(color::Dim, spread.c_str(), rowHeight, 0.5f);
            ImGui::PopFont();
        }

        for (std::size_t row = 0; row < kDepth; ++row) {  // bids: best (highest) at the top
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            if (row >= nBids)
                continue;
            const LevelInfo& l = bids[row];
            const Quantity mine = sim.UserRestingAt(Side::Buy, l.price_);
            ImGui::TableSetColumnIndex(0);
            DepthCell(l.quantity_, mine, barFraction(view.bidBars, l), true, rowHeight);
            ImGui::TableSetColumnIndex(1);
            PriceCell(l.price_, Side::Buy, mine > 0, rowHeight, ticket);
        }
        ImGui::EndTable();
    }
    view.bidBars.Prune(view.frame);
    view.askBars.Prune(view.frame);
    EndCard();
}

// Buy | Sell segmented control. The selected side is filled with its own colour (bid
// green / ask red keep their meaning) and dark text; the other is a quiet control.
void SideToggle(OrderTicket& t, float width) {
    const float half = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    for (const Side side : {Side::Buy, Side::Sell}) {
        const bool selected = t.side == side;
        const ImVec4 c = side == Side::Buy ? color::Bid : color::Ask;
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? c : color::Control);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? c : color::ControlHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(c, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? color::OnAccent : color::Dim);
        if (ImGui::Button(side == Side::Buy ? "Buy" : "Sell", {half, 0}) && !selected) {
            t.side = side;
            t.followBest = true;
        }
        ImGui::PopStyleColor(4);
        if (side == Side::Buy)
            ImGui::SameLine();
    }
}

// True when Enter finished editing the last item (an InputInt's text field).
bool EnterPressedOnLastItem() {
    return ImGui::IsItemDeactivated() &&
           (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
}

void PlaceOrderCard(const Fonts& fonts, viz::LiveSim& sim, OrderTicket& t, ImVec2 size) {
    BeginCard("ticket", size, ImGuiChildFlags_AutoResizeY);
    Caption(fonts, "PLACE ORDER");
    SideToggle(t, ImGui::GetContentRegionAvail().x);

    const bool market = t.Type() == OrderType::Market;
    // While following, the price is the marketable one: best ask for a buy, best bid for a sell.
    const auto best = t.side == Side::Buy ? sim.Book().BestAsk() : sim.Book().BestBid();
    if (t.followBest && best)
        t.price = *best;

    bool enter = false;
    if (ImGui::BeginTable("fields", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, Px(metric::FieldLabelWidth));
        ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthStretch);
        auto label = [](const char* text) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(color::Dim, "%s", text);
            ImGui::TableSetColumnIndex(1);
        };

        label("Type");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##type", ToString(t.Type()).data())) {
            for (int i = 0; i < static_cast<int>(kTicketTypes.size()); ++i) {
                const TicketType& tt = kTicketTypes[static_cast<std::size_t>(i)];
                const std::string item = std::string{ToString(tt.type)} + "  " + tt.help;
                if (ImGui::Selectable(item.c_str(), i == t.typeIndex)) {
                    t.typeIndex = i;
                    t.followBest = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", kTicketTypes[static_cast<std::size_t>(t.typeIndex)].help);

        label("Price");
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Px(metric::BestButtonWidth) - spacing);
        if (market) {
            char text[] = "market";
            ImGui::BeginDisabled();
            ImGui::InputText("##price-market", text, sizeof text, ImGuiInputTextFlags_ReadOnly);
            ImGui::EndDisabled();
        } else {
            if (ImGui::InputInt("##price", &t.price))
                t.followBest = false;
            enter |= EnterPressedOnLastItem();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(market);
        ImGui::PushStyleColor(ImGuiCol_Text, t.followBest ? color::Accent : color::Text);
        if (ImGui::Button("Best", {Px(metric::BestButtonWidth), 0}))
            t.followBest = true;
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Follow the best price: best ask for a buy, best bid for a sell.\n"
                              "Highlighted while following. Typing a price or clicking one\n"
                              "in the ladder stops it.");

        label("Qty");  // same width as Price, so the -/+ buttons line up
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Px(metric::BestButtonWidth) - spacing);
        if (ImGui::InputInt("##qty", &t.qty))
            t.qty = std::max(1, t.qty);
        enter |= EnterPressedOnLastItem();
        ImGui::EndTable();
    }

    // The label says exactly what will be sent.
    char submit[64];
    const char* verb = t.side == Side::Buy ? "Buy" : "Sell";
    if (market)
        std::snprintf(submit, sizeof submit, "%s %d at market###submit", verb, t.qty);
    else
        std::snprintf(submit, sizeof submit, "%s %d @ %d###submit", verb, t.qty, t.price);
    const ImVec4 c = t.side == Side::Buy ? color::Bid : color::Ask;
    ImGui::PushStyleColor(ImGuiCol_Button, c);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Lerp(c, color::Text, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(c, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, color::OnAccent);
    if (ImGui::Button(submit, {-1, 0}) || enter)
        Submit(sim, t);
    ImGui::PopStyleColor(4);
    ImGui::SetItemTooltip("Sends the order now (also: Enter in Price or Qty).\nWorks while the simulation is paused.");

    // Last result: always one line tall, so the card never changes height.
    ImGui::PushFont(fonts.small);
    if (t.result.empty()) {
        ImGui::TextColored(color::Dim, "Tip: click a price in the ladder to use it.");
    } else {
        Dot(t.resultColor);
        ImGui::SameLine(0, Px(6));
        ImGui::TextColored(t.resultColor, "%s", t.result.c_str());
    }
    ImGui::PopFont();
    EndCard();
}

ImVec4 StatusColor(viz::UserStatus s) {
    switch (s) {
        case viz::UserStatus::Resting:
        case viz::UserStatus::PartlyFilled: return color::Accent;  // live: it can still change
        case viz::UserStatus::Filled: return color::Text;
        case viz::UserStatus::Rejected: return color::Ask;
        case viz::UserStatus::Cancelled:
        case viz::UserStatus::Expired: return color::Dim;
    }
    std::unreachable();
}

void MyOrdersCard(const Fonts& fonts, viz::LiveSim& sim, ImVec2 size) {
    BeginCard("orders", size);
    const float lineStart = ImGui::GetCursorPosX();
    const float lineWidth = ImGui::GetContentRegionAvail().x;
    Caption(fonts, "MY ORDERS");
    ImGui::PushFont(fonts.small);
    const char* eod = "End of day";
    const float eodWidth = ImGui::CalcTextSize(eod).x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(lineStart + lineWidth - eodWidth);
    if (ImGui::SmallButton(eod))
        (void)sim.EndOfDay();
    ImGui::SetItemTooltip("Expire every GoodForDay order in the book, yours and the market's.");

    const auto& orders = sim.UserOrders();
    if (orders.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(color::Dim, "No orders yet. Use the ticket above to send one; clicking a "
                                       "price in the ladder fills in the price.");
        ImGui::PopTextWrapPos();
    } else {
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("orders", 7, flags, {0, ImGui::GetContentRegionAvail().y})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Side");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Price");
            ImGui::TableSetupColumn("Filled");
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##cancel");
            ImGui::PushStyleColor(ImGuiCol_Text, color::Dim);
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor();

            // Newest first; only visible rows are built (the list can grow long).
            const float rowHeight = ImGui::GetFrameHeight();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(orders.size()), rowHeight);
            while (clipper.Step())
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const viz::UserOrder& u = orders[orders.size() - 1 - static_cast<std::size_t>(row)];
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(color::Dim, "#%llu",
                                       static_cast<unsigned long long>(u.id - viz::kUserIdBase + 1));
                    ImGui::TableNextColumn();
                    ImGui::TextColored(u.side == Side::Buy ? color::Bid : color::Ask, "%s",
                                       u.side == Side::Buy ? "Buy" : "Sell");
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ToString(u.type).data());
                    ImGui::TableNextColumn();
                    if (u.type == OrderType::Market)
                        ImGui::TextColored(color::Dim, "MKT");
                    else
                        ImGui::Text("%d", u.price);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", u.filled, u.qty);
                    ImGui::TableNextColumn();
                    if (u.status == viz::UserStatus::Rejected && u.reject) {
                        // Red already says "rejected"; the short reason fits, the full one is on hover.
                        ImGui::TextColored(color::Ask, "%s", ShortReason(*u.reject));
                        ImGui::SetItemTooltip("Rejected: %s", ToString(*u.reject).data());
                    } else
                        ImGui::TextColored(StatusColor(u.status), "%s", viz::ToString(u.status).data());
                    ImGui::TableNextColumn();
                    if (u.Live()) {
                        ImGui::PushID(static_cast<int>(u.id - viz::kUserIdBase));
                        if (ImGui::SmallButton("Cancel"))
                            (void)sim.CancelUserOrder(u.id);
                        ImGui::PopID();
                    }
                }
            ImGui::EndTable();
        }
    }
    ImGui::PopFont();
    EndCard();
}

void ChartCard(const Fonts& fonts, ViewState& view, const MotionPolicy& mp, ImVec2 size) {
    BeginCard("chart", size);
    Caption(fonts, "PRICE HISTORY");

    // Y-range: data min/max plus a margin, followed by springs so the chart never snaps
    // when a new high or low enters or leaves the window.
    double lo = std::numeric_limits<double>::infinity();
    double hi = -lo;
    for (std::size_t i = 0; i < view.x.size(); ++i) {
        lo = std::min({lo, view.bandLo[i], view.bandHi[i]});
        hi = std::max({hi, view.bandLo[i], view.bandHi[i]});
        if (!std::isnan(view.last[i])) {
            lo = std::min(lo, view.last[i]);
            hi = std::max(hi, view.last[i]);
        }
    }
    const bool haveData = lo <= hi;
    if (haveData) {
        const double margin = std::max(1.0, (hi - lo) * 0.12);
        const auto targetLo = static_cast<float>(lo - margin);
        const auto targetHi = static_cast<float>(hi + margin);
        if (!view.chartReady) {
            view.chartLo.Snap(targetLo);
            view.chartHi.Snap(targetHi);
            view.chartReady = true;
        } else {
            view.chartLo.Follow(targetLo, mp.dt, motion::ChartResponse, mp.animate);
            view.chartHi.Follow(targetHi, mp.dt, motion::ChartResponse, mp.animate);
        }
    }

    constexpr ImPlotFlags plotFlags =
        ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs;
    if (ImPlot::BeginPlot("##history", ImVec2{-1, -1}, plotFlags)) {
        ImPlot::SetupAxes(nullptr, nullptr,
                          ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_AutoFit,
                          haveData ? ImPlotAxisFlags_None : ImPlotAxisFlags_AutoFit);
        if (haveData)
            ImPlot::SetupAxisLimits(ImAxis_Y1, view.chartLo.value, view.chartHi.value, ImPlotCond_Always);
        ImPlot::SetupLegend(ImPlotLocation_NorthEast, ImPlotLegendFlags_Horizontal | ImPlotLegendFlags_Outside);
        const int n = static_cast<int>(view.x.size());
        if (n > 0) {
            ImPlot::SetNextFillStyle(color::Accent, metric::BandAlpha);
            ImPlot::PlotShaded("bid-ask", view.x.data(), view.bandLo.data(), view.bandHi.data(), n);
            ImPlot::SetNextLineStyle(WithAlpha(color::Bid, 0.8f), 1.0f);
            ImPlot::PlotLine("bid", view.x.data(), view.bandLo.data(), n);
            ImPlot::SetNextLineStyle(WithAlpha(color::Ask, 0.8f), 1.0f);
            ImPlot::PlotLine("ask", view.x.data(), view.bandHi.data(), n);
            ImPlot::SetNextLineStyle(WithAlpha(color::Text, 0.9f), 1.25f);
            ImPlot::PlotLine("last", view.x.data(), view.last.data(), n, ImPlotLineFlags_SkipNaN);
        }
        ImPlot::EndPlot();
    }
    EndCard();
}

void Footer(const Fonts& fonts, const viz::LiveSim& sim) {
    ImGui::PushFont(fonts.small);
    const float gap = ImGui::CalcTextSize("   ").x;
    LabelValue("events", std::to_string(sim.Events()));
    ImGui::SameLine(0, gap);
    LabelValue("resting", std::to_string(sim.Book().Size()));
    ImGui::SameLine(0, gap);
    char fps[16];
    std::snprintf(fps, sizeof fps, "%.0f", ImGui::GetIO().Framerate);
    LabelValue("fps", fps);
    ImGui::SameLine(0, gap);
    // Status vs error: the mismatch is the only bright red text outside the ladder.
    if (sim.Mismatch())
        ImGui::TextColored(color::Ask, "ENGINE / REFERENCE MISMATCH");
    else
        ImGui::TextColored(color::Bid, "engine matches reference");
    ImGui::PopFont();
}

// Writes the current framebuffer as a binary PPM (no image library needed).
bool SaveFramebuffer(const char* path, int width, int height) {
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    std::ofstream out{path, std::ios::binary};
    out << "P6\n" << width << ' ' << height << "\n255\n";
    const std::size_t row = static_cast<std::size_t>(width) * 3;
    for (int y = height - 1; y >= 0; --y)  // OpenGL rows are bottom-up
        out.write(reinterpret_cast<const char*>(rgb.data() + static_cast<std::size_t>(y) * row),
                  static_cast<std::streamsize>(row));
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    const char* screenshotPath = nullptr;
    int screenshotFrames = 240;
    bool reduceMotion = false;
    int speed = 20;    // events per frame
    int scenario = 1;  // Calm
    bool stableBook = true;
    bool demoOrders = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--screenshot" && i + 1 < argc) {
            screenshotPath = argv[++i];
            if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0])))
                screenshotFrames = std::atoi(argv[++i]);
        } else if (arg == "--reduce-motion") {
            reduceMotion = true;
        } else if (arg == "--demo-orders") {
            demoOrders = true;
        } else if (arg == "--raw-book") {
            stableBook = false;
        } else if (arg == "--speed" && i + 1 < argc) {
            speed = std::clamp(std::atoi(argv[++i]), 1, 10'000);
        } else if (arg == "--scenario" && i + 1 < argc) {
            const std::string name = argv[++i];
            const auto it = std::ranges::find(viz::kScenarios, name, &viz::Scenario::name);
            if (it == viz::kScenarios.end()) {
                std::fprintf(stderr, "unknown scenario %s\n", name.c_str());
                return 2;
            }
            scenario = static_cast<int>(it - viz::kScenarios.begin());
        } else {
            std::fprintf(stderr,
                         "usage: %s [--reduce-motion] [--speed N] [--scenario Name] [--raw-book] [--demo-orders] "
                         "[--screenshot out.ppm [frames]]\n",
                         argv[0]);
            return 2;
        }
    }

    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
    });
    if (!glfwInit())
        return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);  // window size follows the display scale
    GLFWwindow* window = glfwCreateWindow(static_cast<int>(metric::WindowWidth),
                                          static_cast<int>(metric::WindowHeight), "Order book viewer",
                                          nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync: one frame per screen refresh
    {
        float xs = 1.0f, ys = 1.0f;
        glfwGetWindowContentScale(window, &xs, &ys);
        gScale = std::max(1.0f, xs);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // don't write imgui.ini into the working directory
    const Fonts fonts = LoadFonts();
    ApplyTheme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    int seed = 1;
    bool running = true;
    viz::LiveSim sim{static_cast<std::size_t>(scenario), static_cast<std::uint64_t>(seed),
                     stableBook};
    ViewState view;
    OrderTicket ticket;
    int frame = 0;       // frames counted for --screenshot
    int frameIndex = 0;  // every frame

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (demoOrders && ++frameIndex == kDemoOrdersFrame)
            PlaceDemoOrders(sim, ticket);
        const std::uint64_t eventsBefore = sim.Events();
        if (running)
            for (int i = 0; i < speed; ++i)
                sim.Step();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(fonts.body);

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("viewer", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---- toolbar: settings that change the simulation reset it the same frame ----
        bool reset = false;
        ImGui::SetNextItemWidth(Px(metric::ComboWidth));
        if (ImGui::BeginCombo("##scenario",
                              viz::kScenarios[static_cast<std::size_t>(scenario)].name.data())) {
            for (int i = 0; i < static_cast<int>(viz::kScenarios.size()); ++i)
                if (ImGui::Selectable(viz::kScenarios[static_cast<std::size_t>(i)].name.data(),
                                      i == scenario)) {
                    scenario = i;
                    reset = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Order-flow regime");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(metric::SeedWidth));
        if (ImGui::InputInt("##seed", &seed))
            reset = true;
        ImGui::SetItemTooltip("Random seed");
        ImGui::SameLine();
        if (ImGui::Checkbox("Stable book", &stableBook))
            reset = true;
        ImGui::SetItemTooltip("Per-order cancels, quotes anchored to the book and liquidity\n"
                              "refills after market orders. Off: the raw test regime.");
        ImGui::SameLine();
        ImGui::Checkbox("Reduce motion", &reduceMotion);
        ImGui::SetItemTooltip("No bar, chart or flash animation. Values are\n"
                              "always exact; only the motion is removed.");

        // Play is the one accent-filled button; light gruvbox text on its blue would be
        // too faint, so it uses the dark background colour.
        ImGui::PushStyleColor(ImGuiCol_Button, running ? color::Control : color::Accent);
        ImGui::PushStyleColor(ImGuiCol_Text, running ? color::Text : color::OnAccent);
        if (ImGui::Button(running ? "Pause" : "Play", ImVec2{Px(metric::PlayWidth), 0}))
            running = !running;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::BeginDisabled(running);
        if (ImGui::Button("Step"))
            sim.Step();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Reset"))
            reset = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderInt("##speed", &speed, 1, 10'000, "%d events / frame", ImGuiSliderFlags_Logarithmic);

        if (reset) {
            sim.Reset(static_cast<std::size_t>(scenario), static_cast<std::uint64_t>(seed), stableBook);
            view.Clear();
            ticket.result.clear();
            ticket.followBest = true;
        }
        view.Observe(sim, ImGui::GetTime());
        if (sim.Events() != eventsBefore || view.x.empty())
            view.Sample(sim);
        const MotionPolicy mp = MotionPolicy::For(reduceMotion, running ? speed : 1, ImGui::GetIO().DeltaTime);

        ImGui::Dummy({0, Px(2)});

        // ---- cards ----
        // Two columns: the market on the left, your orders on the right.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float rightWidth = Px(metric::RightColumnWidth);
        const float fullWidth = ImGui::GetContentRegionAvail().x - rightWidth - style.ItemSpacing.x;
        const float halfWidth = (fullWidth - style.ItemSpacing.x) * 0.5f;
        ImGui::BeginGroup();
        const float topHeight = style.WindowPadding.y * 2 + fonts.small->FontSize + fonts.big->FontSize +
                                3 * ImGui::GetTextLineHeightWithSpacing() + 2 * style.ItemSpacing.y + Px(8);
        PriceCard(fonts, sim, view, mp, {halfWidth, topHeight});
        ImGui::SameLine();
        StatsCard(fonts, sim, {halfWidth, topHeight});

        BookCard(fonts, sim, view, ticket, mp, {fullWidth, 0});

        const float footerHeight = ImGui::GetTextLineHeightWithSpacing();
        const float chartHeight =
            std::max(Px(metric::ChartMinHeight), ImGui::GetContentRegionAvail().y - footerHeight - style.ItemSpacing.y);
        ChartCard(fonts, view, mp, {fullWidth, chartHeight});
        const float chartBottom = ImGui::GetItemRectMax().y;

        Footer(fonts, sim);
        ImGui::EndGroup();

        ImGui::SameLine();
        ImGui::BeginGroup();
        PlaceOrderCard(fonts, sim, ticket, {rightWidth, 0});
        // My orders ends level with the chart, so both columns share a bottom edge.
        const float ordersHeight = std::max(Px(metric::ChartMinHeight), chartBottom - ImGui::GetCursorScreenPos().y);
        MyOrdersCard(fonts, sim, {rightWidth, ordersHeight});
        ImGui::EndGroup();

        ImGui::End();
        ImGui::PopFont();
        ImGui::Render();

        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(color::Background.x, color::Background.y, color::Background.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (screenshotPath && ++frame >= screenshotFrames) {
            const bool ok = SaveFramebuffer(screenshotPath, w, h);
            std::fprintf(stderr, ok ? "saved %s\n" : "could not write %s\n", screenshotPath);
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
