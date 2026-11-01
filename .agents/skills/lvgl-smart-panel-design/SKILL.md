---
name: lvgl-smart-panel-design
description: >-
  LVGL UI design and implementation for NodeLio smart home wall panels:
  S3 480x480 and P4 1280x452, with a warm light appearance and the previous
  曜夜 dark appearance. Use for panel themes, navigation, cards, sliders,
  switches, dialogs and touch interactions. Preserve display_profile geometry
  and embedded rendering budgets.
---

# NodeLio panel design

The panel has two appearances, switched in 显示与声音 and stored as NVS `ui_mode`.
Light is the default. Dark restores the 曜夜 palette from before commit `1053184`.
Both read the same tokens in `ui_theme.cc`. The implementation is LVGL, with
embedded constraints; it is not UIKit or a live-blur renderer.

## Visual decisions

- Read `main/nodelio/ui_theme.h` and `ui_widgets.*` before changing a component.
  Colors and reusable geometry belong in these tokens, never raw widget literals.
  Do not add a third palette or put hex values back into widgets.
- Light: warm canvas `#f3efe6`, opaque white cards, little or no border. Text is
  `#1c1c1e` / `#3c3834` / `#6b6560`. Primary fill is `#1b7a40` so white labels
  clear 4.5:1. Pressed fill is `#176f39`, selection tint `#e5f6eb`. PWM and info
  stay blue `#0077c8`. On-tiles use switch `#d7f3e3`, scene `#f3e8fb`, gpio
  `#ffe8cc`, pwm `#e5f1ff`; off-tiles stay white.
- Dark: canvas `#0c0f14`, surface `#181e28`, brand green `#34c759`, luminous
  on-tiles and slate off-tiles. Do not restyle this set when adjusting light.
- Green means the primary action or an enabled device. Purple denotes scenes,
  amber a warning, red a destructive action. The playback control is a filled
  primary circle of at least 48px; previous, next and stop use the card surface
  with dark glyphs.
- Keep most content neutral. Reserve strong color for the main action and compact
  icon containers. An active home tile uses a light semantic tint, dark title and
  a labeled state badge. Preserve offline, unknown, pending and countdown feedback.
- Use the existing Chinese font and bundled Material Symbols. Do not distribute
  SF Pro or SF Symbols as firmware assets. Preserve all three text-size options;
  use measured line heights and wrapping, never scale bitmap text with transforms.
- Cards use the shared medium radius; sheets the larger radius; compact controls
  and icon containers the small radius. Keep one clear text hierarchy and stable
  alignment, rather than nesting several bordered panels.
- Tabs use a quiet selection capsule in the current palette. Secondary actions
  use a tinted surface and the accent label. Primary actions use the accent fill
  and a contrasting label in both normal and pressed states. Switches use green
  with a white thumb; sliders use a thin track, visible thumb and at least 48
  pixels of effective touch height.
- Check foreground/background contrast after changing tokens. Use `ui_color_on`
  for user-defined solid colors; derive a light tint for custom home backgrounds.
  Essential text targets 4.5:1. State must also be communicated with text or shape.

## Panel-specific invariants

- Geometry comes from `nodelio_display_profile()`: S3 has bottom navigation and a
  square grid; P4 has a side rail and a wider workspace. Do not stretch a square
  modal to impersonate a wide layout or change cloud grid coordinates for style.
- Keep device-specific semantics: PWM retains its value and slider, momentary
  actions don't latch, and DualLine retains two named absolute-state branches.
- Use opaque surfaces instead of live blur, glass sampling or large soft shadows.
  Reuse objects and static transition descriptors. Short 120 ms transitions may
  communicate a touch; avoid repeated bounces, idle animations and full-page fades.
- Preserve retained pages and coalesced updates. LVGL callbacks dispatch network
  work asynchronously. Screen-off must suppress visual work without stopping
  media or breaking touch wake; the first sleep touch must not activate a tile.
- Verify new geometry with actual LVGL for both profiles, including large text,
  disabled and pressed states. Use `scripts/tests/test_panel_ui_lifecycle.py` and
  inspect preview output when useful. Host previews do not prove device frame rate,
  color calibration, current consumption or touch response.

## Primary design references

Reviewed 2026-11-01. These guide hierarchy and interaction; exact token values
are maintained by this firmware for its displays and contrast requirements.

- [Apple HIG: Color](https://developer.apple.com/design/human-interface-guidelines/color)
- [Apple HIG: Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons)
- [Apple HIG: Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
- [Apple HIG: Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)
