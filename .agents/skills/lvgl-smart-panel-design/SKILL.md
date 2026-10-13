---
name: lvgl-smart-panel-design
description: >-
  Specialized LVGL UI/UX design system and C/C++ implementation guidelines for 86-type smart home touch control panels (480x480 resolution). Use this skill when designing, styling, or refactoring LVGL widgets, cards, tiles, themes, sliders, color palettes, typography, and touch micro-interactions.
---

# LVGL 86-Type Smart Panel Design System

This skill defines the visual language, component patterns, and LVGL (v8/v9) C/C++ implementation standards for 86-type smart home control panels (480x480 resolution, e.g., SILA-86, Aqara/MixPad style).

---

## 1. Visual Aesthetics & Design Philosophy

Smart home wall panels are viewed at 0.5m ~ 1.0m distance. They must avoid flat, raw solid colors (which look like engineering test boards) and instead adopt **"Modern Dark Surface with Subtle Luminescence" (深色微质感与呼吸微光)**:

### 1.1 Color Tokens & Depth Palette
- **Canvas Base**: Deep slate green-black (`#16201b` ~ `#1f2b26`), providing a calm, premium ambient backdrop.
- **Card Surface (Off/Idle)**:
  - Fill: Subtle elevated slate (`#25342d`).
  - Border: 1px subtle top/inner light border (`#384c42` / `rgba(255,255,255,0.08)`).
  - Text: Dim muted white (`#d0dfd6`), Secondary hint (`#869d90`).
- **Card Surface (On/Active)**:
  - Fill: Semantic active tint (`#2a5e42` base for switches, `#2b4e68` for PWM, `#5a4525` for GPIO).
  - Accent Highlight: Brand green `#34c759`, PWM cyan `#38bdf8`, Scenario bright emerald `#30d158`.
  - Border: 1.5px glowing accent border (`#6ee08a` / `#7dd3fc` with 60% opacity).
  - Text: Pure white (`#ffffff`).
- **Pressed Feedback**:
  - `LV_STATE_PRESSED`: 120ms smooth ease-out transition, Y offset +1px, background luminance decreases by 15%, border glows.

---

## 2. Card Anatomy & Widget Design Patterns

Every tile must follow a structured, balanced hierarchy rather than scattering naked symbols:

### 2.1 The Icon Capsule Pattern (图标徽章底座)
Never place a naked icon floating above raw text.
- Wrap the icon in a **capsule/circle container** (`32x32px` or `28x28px`, radius 8~10px).
- **Off state**: Capsule background is subtle `rgba(255,255,255,0.06)`, icon color is `#93a89a`.
- **On state**: Capsule background is accent tint `rgba(52,199,89,0.22)`, icon is high-contrast `#ffffff`.

### 2.2 Dual-Line Information Hierarchy (双行信息层级)
- **Top Bar**: Icon Capsule (left) + State Badge (right-top floating pill, e.g. "开", "关", "NC", "点动").
- **Bottom / Mid Content**:
  - **Title**: 15px Puhui font, bold weight, max 8-10 chars with ellipsis.
  - **Sub-hint**: 11-12px secondary status (e.g., "风扇 · 60%", "自锁继电器", "常闭通路").

### 2.3 Semantic Skin Customization (特定设备类型的定制呈现)
1. **PWM / 调光 / 风扇 (Fill Slider Tile)**:
   - Width: 3 columns (full card) or 2 columns (half width).
   - Display: Dedicated icon (`fan` -> `LV_SYMBOL_REFRESH`, `dimmer` -> `LV_SYMBOL_EYE_OPEN`, `heater` -> `LV_SYMBOL_CHARGE`).
   - Track: Sleek 8px height track or fill-level background with smooth knob (`18x18` circular pill).
   - Value: Explicit percentage readout (`风 60%`, `光 80%`).
2. **GPIO 点动 (Momentary Pulse Tile)**:
   - Icon: `LV_SYMBOL_BELL` or trigger pulse glyph.
   - Badge: "点动" (Muted pill).
   - Touch Behavior: Down triggers visual press ripple; release dispatches action without sticking local state to "ON".
3. **DualLine 互斥双段 (Changeover Tile)**:
   - Icon: `LV_SYMBOL_SHUFFLE` (interlocked changeover).
   - Badge: Dynamically shows active branch name (`NC` vs `NO`) based on relay state.

---

## 3. LVGL Implementation Standards (Performance & Stability)

1. **Token Centralization**:
   - All colors and radii MUST be declared in `ui_theme.h`. Absolutely NO raw hex literals in widget rendering files.
2. **Transitions & Animations**:
   - Use `lv_style_transition_dsc_init` for `LV_STYLE_BG_COLOR` and `LV_STYLE_BORDER_COLOR` (120ms~150ms).
   - Prevent allocation churn: Keep transition descriptors static.
3. **Touch Targets**:
   - Minimum interactive area: 48x48 physical pixels.
   - Use `lv_obj_set_ext_click_area(tile, 10)` to absorb slight finger offsets.
4. **Layout Grid Alignment**:
   - For 480x480 resolution: Standard inner canvas height is ~380px (after 30px status bar and 44px tab bar).
   - Optimal grid: 6x6 columns with 4px gap, or 2x2 / 3x2 / 2x3 cards.
