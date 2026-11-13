---
name: apple-design
description: Apple's interface and motion principles (WWDC design talks), adapted to this project's live order-book viewer, a C++ Dear ImGui + ImPlot desktop app (apps/viz/). Use when designing, styling, animating or reviewing the viewer: theme and colour tokens, typography, cards and depth, depth bars, the price flash, the price chart, spring animations, reduced motion, controls and feedback, or any new panel or interaction (e.g. clicking or dragging orders on the ladder).
---

# Apple Design for the order-book viewer

Apple's design principles (chiefly *Designing Fluid Interfaces*, WWDC 2018; *The Details of UI Typography*, WWDC 2020; *Principles of Great Design*), translated from the web/iOS platform to **this** project: a real-time order-book viewer in **C++23, Dear ImGui 1.91, ImPlot 0.16, GLFW + OpenGL3** (`apps/viz/main.cpp`, model in `apps/viz/LiveSim.h`).

The through-line, adapted: **the viewer must feel like a calm, precise instrument.** Data is shown exactly and instantly. Motion is used only on *decoration* (bar lengths, highlights, chart scale) to help the eye follow change; it is continuous, interruptible and never lies about the data.

## 0. Ground rules specific to a live market display

These override everything below when they conflict.

1. **Numbers are never animated.** Prices, quantities, trade and reject counts are drawn at their exact current value every frame. No counting-up tickers, no interpolated prices. A trading screen that shows an in-between value is showing a wrong value.
2. **Layout never moves.** Every card and table has a fixed height. Missing levels are drawn as empty placeholder rows, and every counter line is always present, even at zero. Nothing may shift up or down because the data changed (this was an explicit user requirement).
3. **Colour carries meaning, so it is reserved.** Green = bid / uptick, red = ask / downtick, blue accent = interactive controls, dim grey = labels. Never use green or red for decoration.
   The palette is **Gruvbox Dark** (`color::` in `main.cpp`, written as `Hex(0x...)`): surfaces `bg0_h` < `bg0` < `bg1` (hover `bg2`), values `fg1`, labels `fg4`, accent `blue #83a598`, bid `green #b8bb26`, ask `red #fb4934`. Take new colours from the Gruvbox palette only. Measured contrast on the card: values 10.8:1, labels 5.3:1 (Gruvbox `gray` is only 4.0:1, too faint for 13 px text, so don't use it for labels), bid 7.1:1, ask 4.3:1. Text on an accent-filled surface uses `color::OnAccent` (dark), because `fg1` on blue is 2.0:1.
4. **Motion is decoration only, and it must be truthful.** A depth bar may glide to its new length, but it must arrive in a fraction of a second, and it must be keyed to the **price level**, not the row (see §4).
5. **The viewer must not slow the engine.** Per-frame UI work is O(visible levels). No allocations or O(orders) scans per frame beyond the existing `GetOrderInfos()` snapshot.

## 1. Response — no latency between data and pixels

- Every frame: step the sim, then draw from the book's *current* state. Never draw from a cached or delayed snapshot.
- Controls react immediately. ImGui already gives buttons instant pressed feedback (`ImGuiCol_ButtonActive`); keep that colour visibly different from hover.
- Settings that change the simulation (scenario, seed, Stable book) reset it **the same frame** they are changed. No "Apply" button.
- Avoid any debounce or "wait for animation to finish" on the input path. Play/Pause/Step must always work, even mid-animation.

## 2. Behaviour over animation — springs, in C++

ImGui is immediate-mode: there are no animation objects, so every animated value is a small piece of state stepped once per frame toward a target. Use a **spring**, never a fixed-duration tween. When the target changes mid-motion (the book changed again), the spring simply continues from its current value and velocity: interruptible by construction, with no jump and no velocity "brick wall".

Think in Apple's two parameters:
- **Damping ratio** `ζ`: `1.0` = critically damped, no overshoot. `< 1` overshoots.
- **Response** (seconds): how quickly it gets there. It is not a duration.

Mapping for mass 1: `ω = 2π / response`, stiffness `k = ω²`, damping `c = 2ζω`.

```cpp
// Frame-rate independent spring. Sub-steps keep it stable when a frame is long.
struct Spring {
    float value = 0.0f;
    float velocity = 0.0f;

    void Snap(float v) { value = v; velocity = 0.0f; }

    void Step(float target, float dt, float response = 0.25f, float dampingRatio = 1.0f) {
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
};
// dt = ImGui::GetIO().DeltaTime
```

**House values for this viewer:**

| What | Damping | Response | Why |
|---|---|---|---|
| Depth bar length | `1.0` | `0.15–0.2 s` | must track fast data; no overshoot (an overshooting bar shows a quantity that doesn't exist) |
| Bar scale (the max quantity bars are normalised to) | `1.0` | `0.4 s` | stops every bar jumping when the largest level changes |
| Chart y-range | `1.0` | `0.5 s` | ImPlot's auto-fit snaps; a spring on the limits keeps the chart calm |
| Price flash (colour fade) | ease-out, not a spring | `0.6–0.8 s` | it's a one-shot decay; see §6 |
| Anything the user drags (future) | `0.8` | `0.3 s` | bounce only after real momentum (§9) |

**Never use bounce (`ζ < 1`) on data-driven motion.** Overshoot is only honest after a gesture that carried momentum.

## 3. When motion stops helping

At high simulation speeds (the slider goes to 10,000 events per frame) the data changes completely every frame and any smoothing becomes noise or lag. Rule of thumb: if the target changes more often than the spring's response time, motion is hiding information. Options, in order of preference:
- shorten the response as speed rises, or
- snap (`Spring::Snap`) above a speed threshold.

Always check the viewer at speed 1, ~20 (default) and 10,000.

## 4. Spatial consistency — key animation state to what it represents

> "If something disappears one way, we expect it to emerge from where it came."

- **Key bar springs by price, not by row.** When the best ask moves, every level shifts one row. Row-keyed springs would make a bar morph from its neighbour's length, which is a lie. Use `std::unordered_map<Price, Spring>` per side. A new price level grows from 0, and a level that disappears shrinks to 0 (or is simply dropped, since its row is reused).
- **Bars grow from the price column outward** (bids leftward, asks rightward), and shrink back the same way. This is already the layout in `DepthCell`.
- **Rows do not slide vertically.** At these update rates a sliding ladder would be unreadable. Spatial continuity comes from the fixed price column and the bars, not from moving rows.
- Popups, tooltips and combos open from the control that triggered them (ImGui's default). Keep it that way.

## 5. Hint toward the outcome

Intermediate frames should point at the result: a bar that is shrinking toward zero is a level being consumed. The price flash's direction (▲/▼, green/red) tells the eye where the price went before the number is even read.

## 6. Feedback — status, completion, warning, error

Apple's four kinds of feedback, mapped to the viewer:
- **Status (continuous):** footer line with events, resting orders and fps, plus "engine matches reference" in green.
- **Completion:** Step advances exactly one event, and the number changes visibly.
- **Warning:** rejects are counted per reason, always visible and dim, never alarming.
- **Error:** an engine/reference mismatch is the only thing allowed to use bright red text outside the ladder. It must be impossible to miss.

The **price flash** is feedback on a causal event (a trade at a different price). Rules: it fires on the same frame as the change (harmony), colour and arrow agree (causality), and it decays with an ease-out (`1 - (1-t)^3`) rather than linearly. It fires only on a *change of price*, not on every trade (utility: over-feedback trains the eye to ignore it).

## 7. Materials and depth, without blur

ImGui has no `backdrop-filter`. Depth comes from **surface levels** and **soft shadow**, not translucency:
- Three surface levels, darkest at the back: `color::Background` (window) < `color::Card` (cards) < `color::Control` (buttons, inputs). Interactive things are the lightest surfaces, so they draw the eye.
- Cards: `ChildRounding 10`, 1 px `color::CardBorder`, lifted by a soft shadow of 3–4 expanding rounded rects (alpha ~0.07 each), larger for larger cards. The shadow **must be drawn before `BeginChild`**. Drawn after `EndChild` it lands *on top of* the card background and darkens it (measured: card (30,33,39) became (22,25,29)). `BeginCard` does this, using the card's size from the previous frame for auto-height cards.
- **Never stack translucent on translucent.** The chart's bid–ask band is the only translucent fill (alpha ~0.18), and it sits on a solid card. Bars use a single low-alpha fill over the solid card.
- No hard dividers inside cards where spacing already groups things. Use `ImGui::Separator()` only between unrelated groups.

## 8. Typography — JetBrains Mono, hierarchy from weight and size

The viewer bundles **JetBrains Mono** (CMake fetches it, and `LoadFonts()` loads it from `ORDERBOOK_FONT_DIR` with a fallback to ImGui's font). A monospace face is deliberate here: digits are fixed-width, so prices and quantities line up in columns and don't jitter horizontally as they change. Do not swap it for a proportional face unless that face has tabular figures and they are enabled.

- **Hierarchy = size + weight together** (the `Fonts` struct): `big` 34 px bold for the headline numbers (last price, trades), `body` 16 px regular, `small` 13 px for captions. Use weight before size to add emphasis.
- **Captions** are small, uppercase, dim (`Caption()`). Uppercase at small sizes wants slightly *wider* tracking. ImGui 1.91 has no letter-spacing, so either accept it or draw such captions glyph by glyph with a +0.5–1 px advance. Large numbers would want slightly *tighter* tracking; don't fake that, the bold weight already reads well.
- **Leading:** dense information UI, so keep `ItemSpacing.y` around 8 and table `CellPadding.y` around 3. Don't pad rows out to look "airy".
- **HiDPI:** ImGui fonts are baked into an atlas at fixed pixel sizes. To stay sharp on scaled displays, multiply font sizes by `glfwGetWindowContentScale` and scale the style (`ImGuiStyle::ScaleAllSizes`) once at startup. Never use `SetWindowFontScale` for headline text, because it blurs.

## 9. Direct manipulation (for future interactions)

Natural next features are **clicking a ladder price to place an order** and **dragging a resting order to a new price** (which maps to `OrderBook::ModifyOrder`). If we build them:
- **Highlight on press, commit on release.** Use `ImGui::IsItemActive()` for the pressed state and act on `IsItemDeactivated()`. Allow cancelling by releasing outside the row.
- **Track 1:1 and keep the grab offset.** The dragged marker follows the mouse exactly from where it was grabbed. `ImGui::GetMouseDragDelta()` gives the offset.
- **Use a threshold before a drag counts** (~6–10 px), so a click is never mistaken for a drag.
- **Snap to the tick grid with momentum projection** on release. Take the release velocity (pixels/s, from the last few frames of mouse position), project `current + (v/1000)·d/(1−d)` with `d ≈ 0.99`, snap to the nearest price row, then let a spring (`ζ 0.8`, `0.3 s`) settle the marker there, starting at the release velocity.
- **Rubber-band at boundaries** rather than hard-stopping, e.g. dragging a bid above the best ask would cross the book:
  `f(x) = x·D·0.55 / (D + 0.55·|x|)`, where `x` is the overshoot and `D` the ladder height.
- **Reverse vs. commit:** decide by the sign of the release velocity, not only the position.

## 10. Reduced motion and accessibility

- Provide a **reduce-motion switch** (a checkbox or a `--reduce-motion` flag). When it's on: springs `Snap()`, the price flash becomes a static colour plus arrow with no fade, and the chart limits snap. Meaning is kept and only the motion goes.
- Avoid slow looping oscillations and full-window motion. Nothing in the viewer should move unless the data changed.
- **Colour-blind safety:** green/red is the most common colour-blindness pair. Always pair colour with another cue (side of the ladder, ▲/▼ shape, "Bid"/"Ask" labels). If a palette option is added, offer blue/orange.
- **Contrast:** dim text (`color::Dim`) is for labels only. Values use `color::Text` or the bid/ask colours on `color::Card`.
- Explain jargon with tooltips (`ImGui::SetItemTooltip`), as the Stable book checkbox does.

## 11. Frame-level smoothness

- The viewer runs at vsync (`glfwSwapInterval(1)`). Step springs with `ImGui::GetIO().DeltaTime`, never with an assumed 1/60 s.
- Keep per-frame work bounded: the snapshot, the 5+5 visible levels, and at most `kHistory` (600) chart samples. If the chart history grows, use a ring buffer instead of `vector::erase(begin)`.
- Drawn shapes go through the window draw list. Prefer a few rounded rects over many small primitives.

## 12. Design foundations, applied

1. **Purpose:** the viewer exists to show what the engine is doing: price, top of book, trades and rejects. A new panel must serve that, or it doesn't get built.
2. **Agency:** the user controls scenario, seed, speed and pause. Nothing auto-changes those. Reset is never forced.
3. **Responsibility:** never show a wrong number (rule 0.1). The mismatch indicator exists so the screen cannot silently lie.
4. **Familiarity:** follow trading-screen conventions: bids left/green, asks right/red, asks above bids with the spread in the middle, best prices next to the spread.
5. **Flexibility:** the window must work at its default 640×900 and when resized. The chart takes the remaining height and the cards keep fixed heights.
6. **Simplicity:** the common path (Play and watch) needs no settings. Advanced options (seed, Stable book) are present but quiet.
7. **Craft:** every value is a named token (`color::*`, `Fonts`, `kDepth`, `kHistory`, `kFlashSeconds`). No magic numbers scattered through panels, and alignment is consistent (`AlignedText` with the same inner padding as the bars).
8. **Delight:** the result of the above: a screen that feels calm at speed 1 and composed at speed 10,000.

## 13. Process: how to work on the viewer

1. **Code map:**
   - `apps/viz/main.cpp`:
     - tokens: `color::`, `metric::` (sizes, scaled by `Px()`), `motion::` (responses, speed thresholds)
     - motion: `Spring`, `MotionPolicy` (reduce motion, speed rules), `LevelSprings` (bar springs keyed by price)
     - theme and type: `ApplyTheme`, `LoadFonts`, `TrackedText`/`Caption`, `LabelValue`, `AlignedText`
     - cards: `BeginCard`/`EndCard`, `DrawCardShadow`
     - panels: `PriceCard`, `StatsCard`, `BookCard` with `DepthCell`, `ChartCard`, `Footer`
     - per-view state: `ViewState` (flash, chart history, springs)
   - `apps/viz/LiveSim.h`: the model (no UI code there; keep it that way)
2. **Build:** `cmake --build build -j --target orderbook_viz`. Our code must compile with no warnings.
3. **Look at it, every time.** Render and save a frame, then view it:
   ```bash
   ./build/orderbook_viz --screenshot <scratchpad>/shot.ppm 300
   python3 -I .claude/skills/apple-design/scripts/ppm2png.py <scratchpad>/shot.ppm <scratchpad>/shot.png
   ```
   Then open the PNG with the Read tool. The EGL/ZINK warnings on WSLg are harmless software-rendering fallbacks.
   Flags for the cases to check: `--scenario Calm|Volatile|Crash|Sweep|Poisson`, `--speed N` (events per frame), `--raw-book` (Stable book off: empty sides test the placeholders), `--reduce-motion`.
   When a colour looks wrong, **measure pixels** from the PPM rather than eyeballing the PNG; that is how the shadow-over-card bug was found.
4. **Review motion frame by frame** (Apple's "play it in slow motion"): the simulation is deterministic, so runs with different `--screenshot` frame counts (e.g. 300, 302, 304, 308) show the same moments in time. Crop the same region from each and compare side by side. Check every scenario, `--raw-book`, `--reduce-motion`, and speeds 1, 20 and 10,000. At 10,000 the Debug + ASan build runs at a few fps; that is the simulation, not drawing.
5. **Design interaction and visuals together.** A new panel ships with its colours, spacing, motion and reduced-motion behaviour decided, not styled afterwards.
6. Before finishing, run the tests (`ctest --test-dir build -j4`) if anything outside `apps/viz/` changed.

## Quick reference

| Need | In this viewer | Value |
|---|---|---|
| Data values | drawn exactly, never animated | — |
| Bar length motion | `Spring`, keyed by price | `ζ 1.0`, `0.15–0.2 s` |
| Bar scale / chart range | `Spring` on the max / limits | `ζ 1.0`, `0.4–0.5 s` |
| Price flash | ease-out colour fade, on price change only | `0.6–0.8 s`, `1-(1-t)^3` |
| Bounce | only after a user drag with momentum | `ζ 0.8`, `0.3 s` |
| Fast data | shorten response or snap | at high events/frame |
| Depth | surface levels + soft shadow, no blur | Background < Card < Control |
| Colour | green bid/up, red ask/down, blue controls | reserved meanings |
| Type | JetBrains Mono, 34 bold / 16 / 13 | weight + size together |
| Layout | fixed heights, placeholder rows | nothing jumps |
| Reduced motion | snap springs, static flash | keep meaning, drop motion |
| Verify | `--screenshot`, then `ppm2png.py`, then Read | every visual change |
