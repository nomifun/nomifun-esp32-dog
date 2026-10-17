# Software OLED layout preview and host contracts

These images are **software previews, not device screenshots**. The runner
compiles the unchanged production `display_board.cc`, `oled_display.cc` and
bundled LVGL 9.3 with existing Windows MSVC. It therefore uses the actual base
OLED hierarchy, default styles, alignment, content objects and low-battery
popup. The port lifecycle is a substitute; it creates the same 128 x 64,
full-frame RGB565 render target. After rendering, it executes the unchanged
`_lvgl_port_transform_monochrome` function extracted from the bundled
`esp_lvgl_port_disp.c`. This performs the production blue-channel `> 16`
threshold and inverted vertical-page packing. The extracted function and source
hashes are saved with each run. No device, network, WSL or download is used.

```powershell
python tests/dog_host/run_display_host_tests.py --phase after
```

For every scene the output includes:

- `<scene>_logical_rgb565.bin`: raw 16-bit logical LVGL frame.
- `<scene>_logical.png`: grayscale preview of that logical RGB565 frame.
- `<scene>_oled_pagebits.bin`: 1024 bytes produced by the actual port conversion.
- `<scene>.png`: decoded page bits, where 1 is drawn bright and 0 dark.
- `SOFTWARE_PREVIEW.png`: enlarged logical/converted pairs with a visible warning.
- `geometry.jsonl`, `scrolling.jsonl`, `build.log`, `result.json`: geometry,
  animation positions, diagnostics, scope and source hashes.

The host configuration matches production color depth 16, default theme, DPI,
draw-buffer alignment, large font descriptors and disabled font placeholders.
The complete existing `font_puhui_14_1.c` replaces production's basic built-in
font solely for software layout preview. Assets remain unavailable, so the
early on-device font loader is not exercised; `LvglCBinFont` is an explicit
C-font substitute and its host declaration does not prove cbin loading. Actual
font loading and physical output require a separate on-device panel capture.

`after` asserts menu containment, all seven carousel object slots, counter text
widths including 10/21 through 21/21, visible icon pixels and the five exact
header slots. It also checks six apps, 21 emotions, four music choices and
callbacks, alarm/about fields, settings callback, volume clamps/navigation,
safe reset, menu preservation during status/subtitle refresh, notification
normalization/expiry, selection memory, explicit exit, theme refresh and ten
construction/destruction cycles. Actual LVGL offsets verify that a changed
song begins at its first character, unchanged refresh preserves its position
and elapsed time advances it. Actual GIF validation accepts valid frames/loop
extensions and rejects truncated Netscape extensions.

The ESP/LvglDisplay timer and lock lifecycle, fonts/theme wrappers, image asset
lookup, Application, codec, Settings and hardware interfaces remain substitutes.
Runtime battery/status events, media playback, real NVS/SD failures and physical
button gestures are not reproduced here. The actual low-battery popup exists
but is not triggered by a simulated ADC update. These checks establish host
layout and callback contracts; they do not establish OLED I/O success, controller
polarity, viewing orientation, physical font loading, device timing or usability.
